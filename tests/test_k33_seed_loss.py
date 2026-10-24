#!/usr/bin/env python3
"""K33 acceptance: loss of the original seed; unreachable members stay members.

Real efsd daemons on loopback (tests/k33_daemons.py), private stores under
TMPDIR, free ports (never 17xxx), discovery off; efs-fuse when available.
1. Three nodes form (node 1 is the seed). Mount A goes through node 2;
   files are written.
2. The seed stops (SIGTERM) and stays down. Mount A keeps serving: the old
   files read back and new files are written and read back. Metadata is
   served by nodes 2 and 3 (raft-getattr of the root through node 3).
3. Node 4 joins through node 3 with the seed down: three members of the
   record are up, group 2 moves {1,2,3} -> {2,3,4} with node 1 away (the old
   and the new configuration each have a quorum). Node 5 joins through
   node 4 (a member that joined after the seed went down) and node 6
   through node 5 (storage only: it forwards). Every member that is up
   reports six members; node 1 is listed as down, never dropped.
4. Node 5 is frozen (SIGSTOP) for longer than the heartbeat down threshold,
   then node 6 stops too: with the seed, three members are unreachable and
   all three stay members (six in every table, ids unchanged). Node 5 resumes and node 6 restarts; new files are
   written with only the seed away (three unreachable members exceed the
   one failure 2+1 keeps for writes).
5. The seed restarts from its store: it learns revisions 2 to 4 (six
   members) without any operator step, group 0 voters stay {1,2,3}, node 1
   is retired from group 2. A new mount through node 1 reads every file.
"""
import os, shutil, signal, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload, voters_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-k33s-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_MEMBER_EXIT')}
env['EFS_FAULT_GC_DISABLE'] = '1'
FUSE = fuse_ok()
files, notes, mounts = {}, [], []


def write(m, tag, n=4):
    for k in range(n):
        name, size = f'{tag}-{k}', (1, 5000, 200000, 1 << 20)[k % 4]
        (m.path / name).write_bytes(payload(name, size))
        files[name] = size


def verify(m, what):
    for name, size in files.items():
        assert (m.path / name).read_bytes() == payload(name, size), f'{what}: {name}'


def down_rows(c, i):
    return {k for k, (ep, st) in c.members(i).items() if st == 'down'}


t0 = time.time()
c = Cluster(work, 's', [1, 2, 3, 4, 5, 6], env)
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = None
    if FUSE:
        a = Mount(c, 2, 'mnt-a')
        mounts.append(a)
        write(a, 'f0')

    # 2. The seed goes away and stays away.
    c.stop(1)
    if a:
        verify(a, 'seed down')
        write(a, 'f1')
        verify(a, 'seed down, new files')
    end = time.time() + 30
    while True:
        rc, out = mgmt(env, 'raft-getattr', c.ep(3), '1')
        if rc == 0 or time.time() > end:
            break
        time.sleep(0.3)
    assert rc == 0, out

    # 3. Additions with the seed down.
    c.start(4, join=3)
    c.wait_log(4, r'raft-host: up raft_id=3 n=4 formed', timeout=60)
    c.wait_voters([2, 3, 4], lambda i, v: voters_ok(i, v, 4), timeout=90)
    c.start(5, join=4)
    c.wait_log(5, r'raft-host: up raft_id=4 n=5 formed', timeout=60)
    c.start(6, join=5)
    c.wait_log(6, r'raft-host: up raft_id=5 n=6 formed', timeout=60)
    c.wait_members([2, 3, 4, 5, 6], 6)
    for i in (2, 3, 4, 5, 6):
        assert c.node_id(i) == i, (i, c.node_id(i))
    end = time.time() + 30
    while 1 not in down_rows(c, 3) and time.time() < end:
        time.sleep(0.5)
    assert down_rows(c, 3) == {1}, c.members(3)
    if a:
        write(a, 'f2')
        verify(a, 'after additions with the seed down')

    # 4. Unreachable members stay members.
    c.procs[5].send_signal(signal.SIGSTOP)
    c.stop(6)
    time.sleep(12)
    for i in (2, 3, 4):
        t = c.members(i)
        assert sorted(t) == [1, 2, 3, 4, 5, 6], (i, t)
        assert {1, 6} <= down_rows(c, i), (i, t)
    c.procs[5].send_signal(signal.SIGCONT)
    c.restart(6)
    c.wait_log(6, r'raft-host: up raft_id=5 n=6 formed', timeout=60)
    end = time.time() + 60
    while down_rows(c, 2) != {1} and time.time() < end:
        time.sleep(0.5)
    assert down_rows(c, 2) == {1}, c.members(2)
    if a:
        # Writes need three reachable fragment nodes per stripe (2+1 keeps
        # one failure): with three of six members away a stripe can have two
        # of them, so new files are written once only the seed is away.
        verify(a, 'seed down, nodes 5 and 6 back')
        write(a, 'f3')
        verify(a, 'seed down, new files')

    # 5. The seed returns and learns what it missed.
    c.start(1)
    c.wait_log(1, r'raft-host: (up raft_id=0 n=6 formed|membership revision 4: 6 members)',
               timeout=60)
    c.wait_members([1, 2, 3, 4, 5, 6], 6, timeout=90)
    c.wait_voters([1, 2, 3, 4, 5, 6], lambda i, v: voters_ok(i, v, 6), timeout=90)
    if a:
        b = Mount(c, 1, 'mnt-b')
        mounts.append(b)
        verify(b, 'seed back (mount through node 1)')
        verify(a, 'seed back (mount A)')
        notes.append(f'{len(files)} files exact')
    else:
        notes.append('NOTRUN FUSE payload checks (no /dev/fuse, fusermount3 or efs-fuse)')
finally:
    for i in list(c.procs):
        try:
            c.procs[i].send_signal(signal.SIGCONT)
        except ProcessLookupError:
            pass
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
for n in notes:
    print(n)
print('K33 seed loss: seed down, the cluster serves, nodes 4, 5, 6 join through other members '
      '(group 2 moves with the seed away); frozen and stopped members stay members; the seed '
      f'returns and learns six members: PASS ({time.time() - t0:.1f} s)')
