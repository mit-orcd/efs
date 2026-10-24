#!/usr/bin/env python3
"""K33 acceptance: growth bookkeeping from three to six members.

Real efsd daemons on loopback (tests/k33_daemons.py) and efs-fuse; private
stores under TMPDIR, free ports (never 17xxx), discovery off, GC on (it
runs the W154 rebuild pass and reclaim). Nodes 1-5 have `--quota 1G`; node
6 starts without --quota and takes the automatic quota (safe maximum of its
filesystem: 20% reserve, at least 1 GiB).
At every member count N = 3, 4, 5, 6, through every member:
- readiness: `efs-mgmt status` lists N members, `Cluster state: OK (N up,
  0 down)`, the queried node `state: active`;
- capacity: `Usable bytes ... capacity=` equals the 3-of-N bound over the
  members' quotas (efs_capacity_logical: 2 GiB at three, 2/3 of the sum for
  equal quotas, never more than 2/3 of the sum); the mounted client's
  statfs (df) follows without a remount;
- protection reporting: `Degraded chunks: 0 (group 0: 0, group 2: 0)` once
  both group leaders reported (after group 2 moved to {2,3,4} too) and
  `Abandoned uploads: pending=0`.
At six members:
- reclaim: 24 files of 300 KiB, whose fragments land on the added nodes
  too, are deleted; the fragment files on every store and the used bytes of
  every node return to their values before the files were written;
- W154 rebuild on return: added storage-only node 6 stops (SIGTERM); files
  written at once are acknowledged degraded on two fragments; status counts
  the marks while node 6 is away; node 6 restarts and the count returns to
  0 with rebuilt + retired covering the marks; every file reads back.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import (Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload,  # noqa: E402
                         fragments)

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K33 bookkeeping: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k33b-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_MEMBER_EXIT')}
env.pop('EFS_GC_DISABLE', None)
env['EFS_PACK_MIN_AGE_MS'] = '3600000'  # packing would move the small files mid-check
GiB = 1 << 30
notes = []


def capacity_logical(caps):
    """Python twin of efs_capacity_logical (src/common/placement.c)."""
    caps = [x for x in caps if x]
    if len(caps) < 3:
        return 0
    lo, hi = 0, sum(caps)
    while lo < hi:
        mid = lo + (hi - lo + 1) // 2
        if sum(min(x, mid) for x in caps) >= 3 * mid:
            lo = mid
        else:
            hi = mid - 1
    return lo * 2


def status(i):
    rc, out = mgmt(env, 'status', c.ep(i))
    assert rc == 0, out
    return out


def quota_bytes(c, i):
    """Exact quota of node i from its own startup line (GiB with 2 decimals
    for automatic quotas: the bound is then checked within 0.5%)."""
    m = re.findall(r'Storage quota: (automatic|explicit) ([\d.]+) (GiB|TiB)', c.last_run(i))
    assert m, c.last_run(i)[-2000:]
    kind, v, unit = m[-1]
    return kind, float(v) * (GiB if unit == 'GiB' else 1024 * GiB)


def check_count(n, mnt):
    kinds = {i: quota_bytes(c, i) for i in range(1, n + 1)}
    want = capacity_logical([q for _, q in kinds.values()])
    exact = all(k == 'explicit' for k, _ in kinds.values())
    for i in range(1, n + 1):
        end = time.time() + 60
        while True:
            out = status(i)
            ok = (f'Cluster nodes ({n})' in out and
                  f'Cluster state: OK ({n} up, 0 down)' in out and
                  re.search(rf'Node {re.escape(c.ep(i))} state: active', out) and
                  re.search(r'Degraded chunks: 0 \(group 0: 0, group 2: 0\)', out) and
                  re.search(r'Abandoned uploads: pending=0 ', out))
            if ok or time.time() > end:
                break
            time.sleep(0.5)
        assert ok, f'node {i} at {n} members:\n{out}'
        cap = int(re.search(r'Usable bytes: used=\d+ free=\d+ capacity=(\d+)', out).group(1))
        if exact:
            assert cap == want, (i, n, cap, want)
        else:
            assert abs(cap - want) <= want * 0.005, (i, n, cap, want)
    end = time.time() + 30
    while True:
        st = os.statvfs(mnt.path)
        df = st.f_blocks * st.f_frsize
        if abs(df - cap) <= max(cap * 0.005, 1 << 20) or time.time() > end:
            break
        time.sleep(0.5)
    assert abs(df - cap) <= max(cap * 0.005, 1 << 20), (n, df, cap)
    notes.append(f'{n} members: capacity {cap} B (df {df} B)')
    return cap


t0 = time.time()
c = Cluster(work, 'b', [1, 2, 3, 4, 5, 6], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    for k in range(4):
        (a.path / f'base-{k}').write_bytes(payload(f'base-{k}', 200000))
    caps = [check_count(3, a)]
    assert caps[0] == 2 * GiB, caps
    for n, via in ((4, 2), (5, 3), (6, 5)):
        c.start(n, join=via, quota=None if n == 6 else '1G')
        c.wait_log(n, rf'raft-host: up raft_id={n - 1} n={n} formed', timeout=60)
        if n == 6:
            c.wait_log(6, r'Storage quota: automatic')
        caps.append(check_count(n, a))
    assert caps == sorted(caps) and len(set(caps)) == 4, caps

    # Reclaim at six members.
    os.sync()
    time.sleep(1)
    before = {i: fragments(c.dir / f'n{i}') for i in range(1, 7)}
    names = [f'r-{k}' for k in range(24)]
    for nm in names:
        (a.path / nm).write_bytes(payload(nm, 300 * 1024))
    os.sync()
    mid = {i: fragments(c.dir / f'n{i}') for i in range(1, 7)}
    grew = {i: mid[i] - before[i] for i in mid}
    assert sum(grew[i] for i in (4, 5, 6)) > 0, grew
    for nm in names:
        (a.path / nm).unlink()
    end = time.time() + 90
    while time.time() < end:
        now = {i: fragments(c.dir / f'n{i}') for i in range(1, 7)}
        if now == before:
            break
        time.sleep(1)
    assert now == before, (before, mid, now)
    notes.append(f'reclaim: fragments per node {grew} written, all removed')

    # W154 rebuild on return of an added member.
    c.stop(6)
    names = [f'd-{k}' for k in range(12)]
    errs = []

    def wr(nm):
        try:
            (a.path / nm).write_bytes(payload(nm, 1 << 20))
        except OSError as e:
            errs.append((nm, e))
    ths = [threading.Thread(target=wr, args=(nm,)) for nm in names]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    assert not errs, errs
    acks = len(re.findall(r'degraded acknowledgement', a.logp.read_text(errors='replace')))
    end = time.time() + 30
    marks = None
    while time.time() < end:
        m = re.search(r'Degraded chunks: (\d+) \(', status(2))
        if m and int(m.group(1)) >= max(acks, 1):
            marks = int(m.group(1))
            break
        time.sleep(0.5)
    assert marks, (acks, status(2))
    time.sleep(3)
    m = re.search(r'Degraded chunks: (\d+) \(', status(3))
    assert m and int(m.group(1)) == marks, (marks, status(3))
    c.restart(6, quota=None)
    c.wait_log(6, r'raft-host: up raft_id=5 n=6 formed', timeout=60)
    end = time.time() + 90
    while time.time() < end:
        out = status(2)
        m = re.search(r'Degraded chunks: (\d+) \(.*?rebuilt=(\d+) retired=(\d+)', out)
        if m and int(m.group(1)) == 0:
            break
        time.sleep(1)
    assert m and int(m.group(1)) == 0, out
    rebuilt = sum(len(re.findall(r'raft-host: rebuilt ino=', c.log(i))) for i in range(1, 7))
    assert rebuilt + int(m.group(3)) >= marks, (marks, rebuilt, out)
    for nm in names + [f'base-{k}' for k in range(4)]:
        size = 1 << 20 if nm.startswith('d-') else 200000
        assert (a.path / nm).read_bytes() == payload(nm, size), nm
    notes.append(f'W154: {acks} degraded acknowledgements, {marks} marks, {rebuilt} rebuilt '
                 f'after node 6 returned')
    check_count(6, a)
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
for n in notes:
    print(n)
print('K33 bookkeeping 3->6: readiness, capacity (status and df), protection and abandoned-upload '
      f'reporting, automatic quota, reclaim and W154 rebuild on return: PASS ({time.time() - t0:.1f} s)')
