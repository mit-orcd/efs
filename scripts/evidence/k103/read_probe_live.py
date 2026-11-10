#!/usr/bin/env python3
"""K103 live arm: do compacted Raft followers reject the leader's read probe?

Three private fault-build efsd daemons on loopback (tests/k33_daemons.py) with
EFS_FAULT_SNAP_BYTES small, so every replica compacts its log. A metadata
write load runs until every node has a snapshot in the busiest group (snap>0),
then stops. A read-heavy window follows: parallel efs-bench --meta loops
(stat, getattr and readdir after each create, so ReadIndex rounds on the
metadata leader; the creates in the loop are a small write share).
Counters come from `efs-mgmt raft-status`, cumulative, read before and after
the window: per group `rejects_sent` (follower side, every AppendEntries it
refused) and, on the leader, per peer `rejects`, `sent` (entry batches) and
`next`/`match`. Log lines `why=prev-compacted` are also counted; the daemon
rate-limits those to one per 10 s, so the status counters are the evidence.

Run it from the tree under test; --tree names the build whose daemons run, so
the same script drives the before and the after build:

  python3 scripts/evidence/k103/read_probe_live.py --tree /tmp/efs-taskFG \
      --out /tmp/efs-taskFG-tmp/after.json

Prerequisites: `make test-bins tests/faults/efsd efs-bench efs-mgmt` in --tree.
Cleanup: the work directory (TMPDIR/efs-k103-*) is removed unless --keep.
"""
import argparse, json, os, re, shutil, subprocess, sys, tempfile, threading, time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument('--tree', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--snap-bytes', type=int, default=262144)
ap.add_argument('--read-seconds', type=float, default=30)
ap.add_argument('--readers', type=int, default=4, help='parallel efs-bench metadata loops')
ap.add_argument('--compact-deadline', type=float, default=240)
ap.add_argument('--keep', action='store_true')
a = ap.parse_args()
tree = Path(a.tree).resolve()
sys.path.insert(0, str(tree / 'tests'))
import k33_daemons as k  # noqa: E402

BENCH = tree / 'efs-bench'
for b in (k.EFSD, k.MGMT, BENCH):
    if not b.exists():
        sys.exit(f'{b} missing: make test-bins tests/faults/efsd efs-bench efs-mgmt')

env = dict(os.environ, EFS_FAULT_SNAP_BYTES=str(a.snap_bytes), EFS_TRANSPORT='tcp')
work = Path(tempfile.mkdtemp(prefix='efs-k103-', dir=os.environ.get('TMPDIR')))


def status(c, i):
    rc, out = k.mgmt(env, 'raft-status', c.ep(i))
    assert rc == 0, out
    groups, g = {}, None
    for line in out.splitlines():
        m = re.match(r'\s+group (\d+) hosted=(\d+) role=(\S+)', line)
        if m:
            g = int(m.group(1))
            d = {key: int(v) for key, v in re.findall(r'\b(commit|applied|last|snap|rejects_sent)=(\d+)', line)}
            d['role'] = m.group(3)
            d['peers'] = {}
            groups[g] = d
            continue
        m = re.match(r'\s+peer (\d+) next=(\d+) match=(\d+) .*? sent=(\d+) .*? rejects=(\d+)', line)
        if m and g is not None:
            p = [int(x) for x in m.groups()]
            groups[g]['peers'][p[0]] = {'next': p[1], 'match': p[2], 'sent': p[3], 'rejects': p[4]}
    return groups


def bench(c, *args, timeout=600):
    return subprocess.run([str(BENCH), c.ep(1), *map(str, args)], env=env, text=True,
                          capture_output=True, timeout=timeout)


c = k.Cluster(work, 'k103', [1, 2, 3], env)
result = {'tree': str(tree), 'snap_bytes': a.snap_bytes, 'read_seconds': a.read_seconds}
stop = threading.Event()
try:
    for i in (1, 2, 3):
        c.start(i, join=1 if i > 1 else None)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})

    def writer(w):
        r = 0
        while not stop.is_set():
            r += 1
            bench(c, '--meta', '--files', 2000, '--dirs', 16, '--workers', 16,
                  '--phases', 'mkdir,create', '--keep', '--id', w * 1000000 + r)
    threads = [threading.Thread(target=writer, args=(w,), daemon=True) for w in range(4)]
    for t in threads:
        t.start()
    t0, hot = time.time(), None
    while True:
        st = {i: status(c, i) for i in (1, 2, 3)}
        hot = max(st[1], key=lambda g: st[1][g]['commit'])
        if all(st[i][hot]['snap'] > 0 for i in (1, 2, 3)):
            break
        c.check_alive()
        assert time.time() - t0 < a.compact_deadline, f'no snapshot in group {hot} on every node: {st}'
        time.sleep(1)
    stop.set()
    for t in threads:
        t.join()
    prev = None
    while True:  # quiesce: the hot group's commit stops moving
        st = {i: status(c, i) for i in (1, 2, 3)}
        cur = {i: st[i][hot]['commit'] for i in st}
        if cur == prev:
            break
        prev = cur
        time.sleep(1)
    result['compact_s'] = round(time.time() - t0, 1)
    result['hot_group'] = hot
    before = st
    tb = time.time()
    result['window_start_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(tb))
    deadline = time.time() + a.read_seconds
    outs, lock = [], threading.Lock()

    def reader(w):
        r = 0
        while time.time() < deadline:
            r += 1
            o = bench(c, '--meta', '--files', 3000, '--dirs', 16, '--workers', 16,
                      '--phases', 'mkdir,create,stat,getattr,readdir', '--keep', '--id', (w + 10) * 1000000 + r)
            with lock:
                outs.append(o)
    rd = [threading.Thread(target=reader, args=(w,)) for w in range(a.readers)]
    for t in rd:
        t.start()
    for t in rd:
        t.join()
    ph = {}
    for o in outs:
        for m in re.finditer(r'BENCH_OK kind=meta phase=(\w+) rw=\w+ ops=(\d+) wall_s=([\d.]+)', o.stdout):
            e = ph.setdefault(m.group(1), [0, 0.0])
            e[0] += int(m.group(2))
            e[1] += float(m.group(3))
    result['phases'] = {k_: {'ops': v[0], 'wall_s': round(v[1], 1), 'ops_per_wall_s': round(v[0] / v[1])}
                        for k_, v in ph.items() if v[1]}
    result['read_rc'] = [o.returncode for o in outs]
    assert outs and all(o.returncode == 0 for o in outs), [o.stdout + o.stderr for o in outs]
    result['read_wall_s'] = round(time.time() - tb, 1)
    result['window_end_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    after = {i: status(c, i) for i in (1, 2, 3)}
    nodes = {}
    for i in (1, 2, 3):
        groups = {}
        for g in after[i]:
            b, e = before[i].get(g, {}), after[i][g]
            d = {'role': e['role'], 'snap': e['snap'], 'commit_delta': e['commit'] - b.get('commit', 0),
                 'rejects_sent_before': b.get('rejects_sent', 0), 'rejects_sent_after': e['rejects_sent'],
                 'rejects_sent_delta': e['rejects_sent'] - b.get('rejects_sent', 0), 'peers': {}}
            for p, pe in e['peers'].items():
                pb = b.get('peers', {}).get(p, {'sent': 0, 'rejects': 0, 'next': 0, 'match': 0})
                d['peers'][p] = {'rejects_delta': pe['rejects'] - pb['rejects'], 'sent_delta': pe['sent'] - pb['sent'],
                                 'next_before': pb['next'], 'next_after': pe['next'],
                                 'match_before': pb['match'], 'match_after': pe['match']}
            groups[g] = d
        nodes[i] = {'groups': groups, 'log_prev_compacted_lines': len(re.findall(r'why=prev-compacted', c.log(i)))}
    result['nodes'] = nodes
    result['follower_rejects_in_window'] = sum(
        d['rejects_sent_delta'] for i in nodes for g, d in nodes[i]['groups'].items() if d['role'] != 'LEADER')
    Path(a.out).write_text(json.dumps(result, indent=1, sort_keys=True) + '\n')
    print(json.dumps({k_: result[k_] for k_ in ('compact_s', 'hot_group', 'read_rc', 'follower_rejects_in_window')}))
finally:
    stop.set()
    c.stop_all()
    if not a.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print('work kept at', work)
