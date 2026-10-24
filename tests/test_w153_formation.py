#!/usr/bin/env python3
"""W153: formation commits the membership; count and voters come from it.

Real efsd daemons on loopback, private ports and stores under TMPDIR, the
K33 positional CLI (tests/k33_daemons.py):
1. EFS_MD_RAFT_N set (even empty) stops efsd before it serves; so do the
   removed --node-id/--addr/--port/--storage flags.
2. A seed and one joiner: efs-mgmt raft-mkfs is refused (formation needs
   three) and client metadata is refused before formation.
3. A third node joins; the cluster forms (automatically since K33; the
   optional raft-mkfs agrees). Every node reports group 0 and group 2 on
   voters 0x7; metadata is served.
4. A restart naming another endpoint than the store's is refused.
5. A repeated raft-mkfs keeps the first membership; a restarted member
   derives the count and voters from its store, not the environment.
6. A second efsd on a running member's metadata store is refused (owner lock).
7. A second cluster of five (a seed and four joiners started at once,
   formed automatically, the rest added as they retry HELLO): group 0 on
   {1,2,3}, group 2 on {2,3,4}; node 5 hosts neither group.
"""
import os, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, HOST, free_port, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-w153-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if k not in ('EFS_MD_RAFT_N', 'EFS_FAULT_MEMBER_EXIT')}
env['EFS_FAULT_GC_DISABLE'] = '1'


def mgmt(*args):
    return _mgmt(env, *args)


def voters(c, i):
    return {g: (int(bool(h)), v) for g, (h, v) in c.status(i).items()}


def mkfs_until_ok(c, i, timeout=40):
    end, out = time.time() + timeout, ''
    while time.time() < end:
        rc, out = mgmt('raft-mkfs', c.ep(i))
        if rc == 0 and 'raft-mkfs rc=0 ' in out:
            return out
        time.sleep(0.5)
    raise AssertionError('raft-mkfs never succeeded: ' + out + '\n' + c.last_run(i)[-3000:])


t0 = time.time()
clusters = []
try:
    # 1. The removed variable and flags stop efsd.
    c = Cluster(work, 'refuse', [1], env)
    clusters.append(c)
    for value in ('3', ''):
        p = c.start(1, extra_env={'EFS_MD_RAFT_N': value})
        assert p.wait(timeout=30) == 1, c.log(1)
        assert 'EFS_MD_RAFT_N was removed' in c.last_run(1), c.last_run(1)[-800:]
        assert 'raft-host: up' not in c.last_run(1), c.last_run(1)[-800:]
        c.procs.pop(1)
    for flag in ('--node-id', '--addr', '--port', '--storage'):
        p = subprocess.run([str(EFSD), flag, '1', c.ep(1), str(c.dir / 'x')], capture_output=True,
                           text=True, env=env, timeout=30)
        assert p.returncode == 1 and f'{flag} was removed (K33)' in p.stderr, (flag, p.stderr[-600:])

    # 2. A seed and one joiner: formation refused, nothing served.
    c = Cluster(work, 'three', [1, 2, 3, 4], env)
    clusters.append(c)
    c.start(1)
    c.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    c.start(2, join=1)
    c.wait_log(2, r'join: waiting at .*: cluster [0-9a-f]+ is forming \(2 joined; formation '
               r'needs 3\)')
    rc, out = mgmt('raft-mkfs', c.ep(1))
    assert rc == 1 and 'raft-mkfs rc=-' in out, out
    c.wait_log(1, r'raft-mkfs refused: formation needs 3 members; 2 joined this seed')
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc != 0, out
    c.wait_log(1, r'unready: formation needs 3 members \(joined: this seed at')

    # 3. Third node; formation commits three members.
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members, g0 voters 0x7, g2 voters 0x7, g2 hosted')
        assert c.node_id(i) == i, (i, c.last_run(i)[-1500:])
    out = mkfs_until_ok(c, 1)
    for i in (1, 2, 3):
        assert voters(c, i) == {0: (1, 7), 2: (1, 7)}, (i, voters(c, i))
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc == 0, out

    # 4. A member keeps its endpoint: a restart naming another is refused.
    c.stop(3)
    p = subprocess.run([str(EFSD), c.ep(4), str(c.dir / 'n3'), '--quota', '1G',
                        '--discovery-port', '0'], capture_output=True, text=True, env=env,
                       timeout=30)
    assert p.returncode == 1 and 'a member keeps its endpoint' in p.stderr, p.stderr[-800:]
    c.start(3, join=1)
    c.wait_log(3, r'raft-host: up raft_id=2 n=3 formed')

    # 5. A repeated mkfs keeps the first membership; a restart derives the
    # count and voters from the member store.
    mkfs_until_ok(c, 2)
    c.stop(2)
    c.start(2, join=1)
    c.wait_log(2, r'formed \(3 committed members\)')
    c.wait_log(2, r'raft-host: up raft_id=1 n=3 formed')
    assert voters(c, 2) == {0: (1, 7), 2: (1, 7)}, voters(c, 2)

    # 6. A second daemon on node 3's metadata store (its data root is
    # elsewhere, so the storage-root lock does not stop it first) is
    # refused by the member store's owner lock.
    (c.dir / 'other').mkdir()
    p = subprocess.run([str(EFSD), '--join', c.ep(1), '--listen', c.ep(4), str(c.dir / 'other'),
                        '--meta-storage', str(c.dir / 'n3'), '--quota', '1G',
                        '--discovery-port', '0'], capture_output=True, text=True, env=env,
                       timeout=30)
    assert p.returncode == 1 and 'owned by another running process' in p.stderr, p.stderr[-800:]
    c.stop_all()

    # 7. Five members: the committed count selects the D38 layout.
    c = Cluster(work, 'five', [1, 2, 3, 4, 5], env)
    clusters.append(c)
    c.start(1)
    c.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    for i in range(2, 6):
        c.start(i, join=1)
    for i in range(1, 6):
        c.wait_log(i, r'raft-host: (formed|membership revision \d+): 5 members', timeout=60)
    ids = {c.node_id(i) for i in range(1, 6)}
    assert ids == {1, 2, 3, 4, 5}, ids
    by_id = {c.node_id(i): i for i in range(1, 6)}
    # Node 1 keeps a retired group-2 replica when the cluster formed at
    # three and grew (K33 M6), or hosts none when it formed at four or
    # five; the others are exact.
    want = {2: {0: (1, 7), 2: (1, 0xe)}, 3: {0: (1, 7), 2: (1, 0xe)},
            4: {0: (0, 7), 2: (1, 0xe)}, 5: {0: (0, 7), 2: (0, 0xe)}}
    end = time.time() + 60
    while time.time() < end and any(voters(c, by_id[n]) != want[n] for n in want):
        time.sleep(0.3)
    for n in want:
        assert voters(c, by_id[n]) == want[n], (n, voters(c, by_id[n]))
    v1 = voters(c, by_id[1])
    assert v1[0] == (1, 7) and v1[2][0] == 0, v1
finally:
    for c in clusters:
        c.stop_all()
    if os.environ.get('EFS_W153_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print('W153 formation: EFS_MD_RAFT_N and the removed id/endpoint flags refused; 2 members '
      'refused (raft-mkfs says formation needs 3); 3 committed (g0/g2 voters 0x7 everywhere, ids '
      '1..3 in endpoint order); metadata refused before formation; another endpoint refused; '
      'repeat mkfs and restart keep the membership; store lock; 5 members (g0 0x7, g2 0xe, '
      f'node 5 hosts none): PASS ({time.time() - t0:.1f} s)')
