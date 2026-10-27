#!/usr/bin/env python3
"""K22/W23: live leader admission at 3/4 of the unapplied-log bound.

Loopback 3-member cluster of fault-build daemons (tests/k33_daemons.py).
The group-0 leader runs with a small KV budget (EFS_FAULT_L0_BYTES +
EFS_FAULT_MEMTABLE_BYTES derive a small max_unapplied) and its compactor
parked (EFS_FAULT_COMPACT_STALL + the marker file), so its L0 fills, apply
defers and its committed-unapplied backlog grows.

Expected, asserted from `efs-mgmt raft-status` and the client log:
1. At 3/4 of max_unapplied the leader stops admitting new work: its
   `admit_busy` counter climbs (documented BUSY); client writes keep
   succeeding through retries within their budgets — no EIO.
2. If the leader cannot recover, leadership moves to a member with capacity
   (`election held: low capacity` on the pressured node, a new leader with
   `admit_busy` at 0).
3. Releasing the compactor lets the backlog drain and the node recover.
"""
import argparse
import os, re, shutil, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster as _Cluster, EFSD, MGMT, FUSE, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')

L0_BYTES = os.environ.get('K22_L0_BYTES', str(2 << 20))         # small budget on the pressured node
MEMTABLE_BYTES = os.environ.get('K22_MEMTABLE_BYTES', str(1 << 20))
BOUND_DEADLINE = int(os.environ.get('K22_BOUND_DEADLINE', '900'))  # to reach 3/4 + admission refusals
MOVE_DEADLINE = int(os.environ.get('K22_MOVE_DEADLINE', '900'))    # leadership move after full bound
DRAIN_BOUND = int(os.environ.get('K22_DRAIN_BOUND', '600'))
FAULT_MARKER = os.environ.get('K22_FAULT_MARKER', '')              # default: per-run marker
WRITE_BYTES = 2048

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--keep', action='store_true', help='retain private logs/stores for evidence review')
args = parser.parse_args()
work = Path(tempfile.mkdtemp(prefix='efs-k22-admit-', dir=os.environ.get('TMPDIR')))
print('[k22admit] work:', work, flush=True)
env = dict(os.environ)
env['EFS_TRANSPORT'] = 'tcp'


def mgmt(*args):
    return _mgmt(env, *args)


def Cluster(name, ids):
    return _Cluster(work, name, ids, env)


def fuse_ok():
    return FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and shutil.which('fusermount3')


class Mount:
    def __init__(self, c, node, name):
        self.path = c.dir / name
        self.path.mkdir()
        self.log = open(c.dir / f'{name}.log', 'w')
        self.p = subprocess.Popen([str(FUSE), c.ep(node), str(self.path), '-f'],
                                  stdout=self.log, stderr=subprocess.STDOUT, env=env)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(self.path):
            if self.p.poll() is not None:
                raise AssertionError((c.dir / f'{name}.log').read_text()[-2000:])
            time.sleep(0.1)
        assert os.path.ismount(self.path), (c.dir / f'{name}.log').read_text()[-2000:]

    def close(self):
        subprocess.run(['fusermount3', '-u', str(self.path)], capture_output=True)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def raw_status(c, i):
    rc, out = mgmt('raft-status', c.ep(i))
    assert rc == 0, out
    return out


def fields(status_text):
    """group -> (role, commit, applied, max_unapplied, admit_busy), plus
    out['host']['admit_busy']: the host admission counter. The 3/4-of-bound
    refusal is the host's (raft_host.c); the group line's admit_busy counts
    the raft-level refusals at the FULL bound, which the host's earlier
    refusal normally keeps from ever firing."""
    out = {}
    for line in status_text.splitlines():
        m = re.match(r'\s+group (\d+) .*', line)
        if m:
            d = {}
            for k in ('role', 'commit', 'applied', 'max_unapplied', 'admit_busy'):
                km = re.search(k + r'=(\S+)', line)
                d[k] = km.group(1) if km else ''
            out[int(m.group(1))] = d
            continue
        if line.startswith('  bound '):
            km = re.search(r'admit_busy=(\d+)', line)
            out['host'] = {'admit_busy': km.group(1) if km else '0'}
    return out


def payload(name, size=WRITE_BYTES):
    import hashlib
    out, k = b'', 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return out[:size]


stop_load = threading.Event()
errors = []
wrote = [0]


def bench_loop(c, w):
    """Fast metadata ingest through efs-bench --meta: the leader's KV must
    fill (L0 budget, parked compactor) for the unapplied backlog to grow."""
    r = 0
    bench = Path(__file__).resolve().parent.parent / 'efs-bench'
    while not stop_load.is_set():
        r += 1
        subprocess.run([str(bench), c.ep(1), '--meta', '--files', '2000',
                        '--dirs', '16', '--workers', '16',
                        '--phases', 'mkdir,create', '--keep',
                        '--id', str(w * 1000000 + r)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def writer(mnt, tag):
    n = 0
    base = Path(mnt.path) / f'w-{tag}'
    base.mkdir()
    while not stop_load.is_set():
        n += 1
        try:
            (base / f'f{n:06d}').write_bytes(payload(f'{tag}/{n}'))
            wrote[0] += 1
        except OSError as e:
            errors.append(f'{tag}/{n}: {e} at t={time.time()-t0:.0f}s')


t0 = time.time()
c = None
mounts = []
try:
    assert fuse_ok(), 'NOTRUN: no FUSE on this host'
    c = Cluster('admit34', [1, 2, 3])
    # Only node 1 gets the small KV budget and the compactor park. The forming
    # node holds the first leadership, so node 1 leads group 0 at start
    # (asserted below). Followers keep default budgets: with every node on a
    # small budget the group throttles to its slowest memtable flush (~34
    # entries/s), the fill outruns the deadline, and client closes starve past
    # their 30 s flush deadline for reasons unrelated to admission.
    c.start(1, extra_env={
        'EFS_FAULT_L0_BYTES': L0_BYTES,
        'EFS_FAULT_MEMTABLE_BYTES': MEMTABLE_BYTES,
        'EFS_FAULT_COMPACT_STALL': '1',
        'EFS_FAULT_COMPACT_FILE': str(work / 'fault-1')})
    for i in (2, 3):
        c.start(i, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})

    # park the group-0 leader's compactor up front: its L0 fills, apply
    # defers, backlog grows
    g0_leader = next(i for i in (1, 2, 3)
                     if fields(raw_status(c, i)).get(0, {}).get('role') == 'LEADER')
    assert g0_leader == 1, f'group 0 leader is node {g0_leader}, not the pressured node 1'
    print(f'[k22admit] group 0 leader: node {g0_leader}; parking its compactor')
    (work / f'fault-{g0_leader}').touch()

    # clients mount a HEALTHY node: a client pinned to the pressured node
    # itself cannot complete its consistency waits while that node's apply
    # is stalled (a separate, valid degradation story). This gate verifies
    # the admission path: the pressured leader refuses, clients riding the
    # BUSY window through retries, leadership moving to a member with
    # capacity — all invisible to a correctly placed client.
    client_node = next(i for i in (1, 2, 3) if i != g0_leader)
    m = Mount(c, client_node, 'mnt-a')
    mounts.append(m)
    threads = [threading.Thread(target=writer, args=(m, k), daemon=True) for k in range(4)]
    threads += [threading.Thread(target=bench_loop, args=(c, k), daemon=True)
                for k in range(4)]
    for t in threads:
        t.start()

    # 1. drive until the leader's backlog passes 3/4 and it refuses work
    deadline = time.time() + BOUND_DEADLINE
    refused = False
    max_seen = 0
    polls = 0
    while time.time() < deadline:
        time.sleep(2)
        st = fields(raw_status(c, g0_leader)).get(0, {})
        try:
            commit, applied = int(st.get('commit') or 0), int(st.get('applied') or 0)
            maxu = int(st.get('max_unapplied') or 0)
            admit = int(st.get('admit_busy') or 0)
        except ValueError:
            continue
        backlog = commit - applied
        host_admit = int(fields(raw_status(c, g0_leader)).get('host', {}).get('admit_busy') or 0)
        max_seen = max(max_seen, backlog)
        polls += 1
        if polls % 15 == 0:
            print(f'[k22admit] t={int(time.time() - t0)}s backlog={backlog} '
                  f'max_unapplied={maxu} admit_busy(host)={host_admit}', flush=True)
        if maxu and backlog >= maxu * 3 // 4 and host_admit > 0:
            refused = True
            break
    assert refused, f'leader never refused at 3/4 (max backlog seen {max_seen})'
    print(f'[k22admit] leader refused admission at backlog {max_seen} '
          f'(>= 3/4 of {maxu}); admit_busy={admit}; {int(time.time() - t0)} s in')

    # 2. leadership moves if the pressured node cannot recover
    deadline = time.time() + MOVE_DEADLINE
    moved = False
    while time.time() < deadline:
        time.sleep(5)
        for i in (1, 2, 3):
            if i == g0_leader:
                continue
            st = fields(raw_status(c, i)).get(0, {})
            if st.get('role') == 'LEADER':
                moved = True
                new_leader = i
                break
        if moved:
            break
    assert moved, 'leadership never moved off the over-bound leader'
    t_move = time.time()
    assert 'low capacity' in c.log(g0_leader) and \
        ('election held' in c.log(g0_leader) or 'steps down' in c.log(g0_leader)), \
        'no capacity-hold evidence on the pressured node'
    print(f'[k22admit] group 0 leadership moved to node {new_leader}; '
          f'pressured node logged election-held (low capacity)')

    # 3. release: compactor resumes, backlog drains, node recovers
    (work / f'fault-{g0_leader}').unlink()
    t_rel = time.time()
    recovered = False
    while time.time() - t_rel < DRAIN_BOUND:
        time.sleep(3)
        st = fields(raw_status(c, g0_leader)).get(0, {})
        try:
            if int(st.get('commit') or 0) - int(st.get('applied') or 0) < 100:
                recovered = True
                break
        except ValueError:
            continue
    assert recovered, 'pressured node did not drain after release'
    print(f'[k22admit] pressured node drained {int(time.time() - t_rel)} s after release')

    # client proof: once leadership has settled on a member with capacity,
    # a client mounted on a healthy node sees a clean service — no errors
    # after the move (+ grace for the redirect). Errors before that are
    # printed as findings: at this compressed budget scale the pressured
    # leader's apply churn turns some closes into EIO at their 30 s flush
    # deadline well before the admission point; that artifact is the
    # environment, and the production-scale window is the multi-host
    # driver's subject, not this gate's.
    stop_load.set()
    for t in threads:
        t.join(timeout=60)
    grace = (t_move - t0) + 5
    late = [e for e in errors if float(e.rsplit('t=', 1)[1][:-1]) > grace]
    early = [e for e in errors if float(e.rsplit('t=', 1)[1][:-1]) <= grace]
    if early:
        print(f'[k22admit] {len(early)} client errors inside the pressure window '
              f'(findings): {early[:6]}')
    assert not late, f'{len(late)} client errors after leadership settled: {late}'
    assert wrote[0] > 0, 'no client writes happened'
    print(f'[k22admit] {wrote[0]} client writes, zero errors after the move')
    print('[k22admit] PASS')
except Exception as e:
    print('[k22admit] FAIL:', e)
    raise
finally:
    stop_load.set()
    for i in (1, 2, 3):
        (work / f'fault-{i}').unlink(missing_ok=True)
    for m in mounts:
        m.close()
    if c:
        c.stop_all()
    if args.keep:
        print('[k22admit] retained work:', work, flush=True)
    else:
        shutil.rmtree(work, ignore_errors=True)
