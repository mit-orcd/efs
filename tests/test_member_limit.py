#!/usr/bin/env python3
"""F14: a 17th joiner is refused while the 16 members keep working.

Behavior (EFS_MAX_NODES = 16): a formed cluster of 16 members answers a
joining store's HELLO with EFS_HELLO_REJECT_FULL instead of queueing an
addition that cannot commit. The member logs `refused: the cluster has the
maximum of 16 members`; the joiner logs `the cluster has the maximum of 16
members` and exits 1 without a node id; the membership and the voter sets
stay as they were.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build);
private stores under TMPDIR, free ports (never 17xxx). Nodes 1-3 form the
cluster, nodes 4-16 join at once through members 1-3; node 17 then joins
through node 2. Afterwards all 16 report 16 members and the same voters;
with FUSE, a mount through node 16 writes and reads files.
"""
import os, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-f14-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
t0 = time.time()
ids = list(range(1, 18))
c = Cluster(work, 'm', ids, env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    for i in range(4, 17):
        c.start(i, join=1 + i % 3)
    full = list(range(1, 17))
    c.wait_members(full, 16, timeout=300)
    before = {i: c.status(i) for i in (1, 2, 3, 4, 16)}
    print(f'16 members formed and added ({time.time() - t0:.1f} s)')

    c.start(17, join=2)
    rc = c.wait_exit(17, timeout=60)
    run = c.last_run(17)
    assert rc == 1, (rc, run[-2000:])
    assert 'the cluster has the maximum of 16 members' in run, run[-2000:]
    assert 'Joined cluster' not in run and 'listening on' not in run, run[-2000:]
    c.wait_log(2, r'joining store [0-9a-f]+ at 127\.0\.0\.1:\d+ refused: the cluster has the '
                  r'maximum of 16 members', run=False)
    tab = c.wait_members(full, 16, timeout=60)
    assert c.ep(17) not in tab.values(), tab
    after = {i: c.status(i) for i in before}
    assert {i: {g: v[1] for g, v in s.items()} for i, s in after.items()} == \
        {i: {g: v[1] for g, v in s.items()} for i, s in before.items()}, (before, after)
    assert all(c.alive(i) for i in full), [i for i in full if not c.alive(i)]
    if fuse_ok():
        a = Mount(c, 16, 'mnt')
        mounts.append(a)
        for k in range(8):
            (a.path / f'f{k}').write_bytes(payload(f'f{k}', 200000))
        for k in range(8):
            assert (a.path / f'f{k}').read_bytes() == payload(f'f{k}', 200000)
    print(f'17th joiner refused (exit 1, clear message), 16 members unchanged'
          f'{", a mount through node 16 writes and reads" if mounts else " (no FUSE)"} PASS')
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_F14_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'member limit: PASS ({time.time() - t0:.1f} s)')
