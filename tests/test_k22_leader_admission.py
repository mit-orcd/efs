#!/usr/bin/env python3
"""K22/W23: live leader admission and handoff at local capacity pressure.

Loopback 3-member cluster of fault-build daemons (tests/k33_daemons.py).
The group-0 leader runs with a small KV budget (EFS_FAULT_L0_BYTES +
EFS_FAULT_MEMTABLE_BYTES derive a small max_unapplied) and its compactor
parked (EFS_FAULT_COMPACT_STALL + the marker file), so its L0 fills, apply
defers and its committed-unapplied backlog grows.

Expected, asserted from `efs-mgmt raft-status` and the client log:
1. At KV pressure or 3/4 of max_unapplied the leader stops admitting new work: its
   `admit_busy` counter climbs (documented BUSY); client writes keep
   succeeding through retries within their budgets — no EIO.
2. If the leader cannot recover, leadership moves to a member with capacity
   (`election held: low capacity` on the pressured node, a new leader with
   `admit_busy` at 0).
3. Releasing the compactor lets the backlog drain and the node recover.
"""
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

work = Path(tempfile.mkdtemp(prefix='efs-k22-admit-', dir=os.environ.get('TMPDIR')))
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


def leader_on(c, target, timeout=60):
    """Establish fixture leadership, not an assumed election outcome."""
    end, last = time.time() + timeout, {}
    while time.time() < end:
        last = {i: fields(raw_status(c, i)).get(0, {}) for i in (1, 2, 3)}
        leaders = [i for i, st in last.items() if st.get('role') == 'LEADER']
        if leaders == [target]:
            return target
        if len(leaders) == 1:
            rc, out = mgmt('raft-step-down', c.ep(leaders[0]), '0', str(c.node_id(target)))
            print(f'[k22admit] fixture transfer to node {target}: {out.strip()}', flush=True)
            # BUSY while the target catches up is fixture setup, before load.
            assert rc == 0 or re.search(r'rc=-(?:13|15)\b', out), out
        time.sleep(0.2)
    raise AssertionError(f'fixture leadership never moved to node {target}: {last}')


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
    # node may lose leadership during formation. Establish node 1 as leader
    # explicitly before pressure. Followers keep default budgets: with every node on a
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
    # Exercise setup after a real leadership change too: bootstrap leadership
    # is not a contract, and the old fixture failed when node 2 won naturally.
    leader_on(c, 2)
    g0_leader = leader_on(c, 1)
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

    # 1. Drive to the first capacity bound. A full L0 already refuses
    # publication, so it must trigger handoff before the log also fills.
    deadline = time.time() + BOUND_DEADLINE
    refused = False
    max_seen = 0
    polls = 0
    while time.time() < deadline:
        time.sleep(2)
        current = raw_status(c, g0_leader)
        parsed = fields(current)
        st = parsed.get(0, {})
        try:
            commit, applied = int(st.get('commit') or 0), int(st.get('applied') or 0)
            maxu = int(st.get('max_unapplied') or 0)
            admit = int(st.get('admit_busy') or 0)
        except ValueError:
            continue
        backlog = commit - applied
        host_admit = int(parsed.get('host', {}).get('admit_busy') or 0)
        kv = {k: int(v) for k, v in re.findall(r'(kv_\w+)=(\d+)', current)}
        l0_full = kv.get('kv_l0_cap', 0) > 0 and kv.get('kv_l0_bytes', 0) >= kv['kv_l0_cap']
        ram_full = kv.get('kv_mem_budget', 0) > 0 and \
            kv.get('kv_mt_bytes', 0) >= kv['kv_mem_budget'] * 3 // 4
        log_full = maxu > 0 and backlog >= maxu * 3 // 4
        max_seen = max(max_seen, backlog)
        polls += 1
        if polls % 15 == 0:
            print(f'[k22admit] t={int(time.time() - t0)}s backlog={backlog} '
                  f'max_unapplied={maxu} admit_busy(host)={host_admit}', flush=True)
        if (l0_full or ram_full or log_full) and host_admit > 0:
            refused = True
            break
    assert refused, f'leader never refused at local capacity (max backlog seen {max_seen})'
    print(f'[k22admit] leader refused admission: L0_full={l0_full} RAM_watermark={ram_full} '
          f'log_watermark={log_full} backlog={backlog}/{maxu}; '
          f'admit_busy={host_admit}; {int(time.time() - t0)} s in', flush=True)

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

    # Client service must survive the entire pressure window, not just the
    # period after leadership moves. EIO before handoff is still a failure.
    stop_load.set()
    for t in threads:
        t.join(timeout=60)
    assert not errors, f'{len(errors)} client errors during pressure/handoff: {errors}'
    assert wrote[0] > 0, 'no client writes happened'
    print(f'[k22admit] {wrote[0]} client writes, zero errors throughout pressure and handoff')
    print('[k22admit] PASS')
except Exception as e:
    print('[k22admit] FAIL:', e)
    raise
finally:
    failed = sys.exc_info()[0] is not None
    stop_load.set()
    for i in (1, 2, 3):
        (work / f'fault-{i}').unlink(missing_ok=True)
    for m in mounts:
        m.close()
    if c:
        c.stop_all()
    if failed:
        print(f'[k22admit] failed fixture retained: {work}', flush=True)
    else:
        shutil.rmtree(work, ignore_errors=True)
