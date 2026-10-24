#!/usr/bin/env python3
"""W153: formation commits the membership; count and voters come from it.

Real efsd daemons on loopback, private ports and stores under TMPDIR:
1. EFS_MD_RAFT_N set (even empty) stops efsd before it serves.
2. Two joined members: efs-mgmt raft-mkfs is refused and names node 3 as
   missing; client metadata is refused before formation.
3. A third member joins; the cluster forms (automatically since K33; the
   optional raft-mkfs agrees). Every node reports group 0 and group 2 on
   voters 0x7; metadata is served.
4. A node whose id is not the next one (5 while 4 is absent) is pending:
   it is told so, stays out of the node table and keeps retrying (K33).
5. A repeated raft-mkfs keeps the first membership; a restarted member
   derives the count and voters from its store, not the environment.
6. A second efsd on a running member's metadata store is refused (owner lock).
7. A second cluster of five (formed automatically at three or more, the
   rest added as they retry HELLO; raft-mkfs is optional): group 0 on
   {1,2,3}, group 2 on {2,3,4}; node 5 hosts neither group.
"""
import os, re, shutil, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
EFSD, MGMT = root / 'efsd', root / 'efs-mgmt'
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
HOST = '127.0.0.1'
work = Path(tempfile.mkdtemp(prefix='efs-w153-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if k != 'EFS_MD_RAFT_N'}
env['EFS_GC_DISABLE'] = '1'


def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


class Cluster:
    def __init__(self, name, n):
        self.dir = work / name
        self.dir.mkdir()
        self.ports = {i: free_port() for i in range(1, n + 1)}
        self.procs = {}

    def ep(self, i):
        return f'{HOST}:{self.ports[i]}'

    def start(self, i, join=None, extra_env=None):
        store = self.dir / f'n{i}'
        store.mkdir(exist_ok=True)
        args = [str(EFSD), '--node-id', str(i), '--addr', HOST, '--port', str(self.ports[i]),
                '--storage', str(store), '--quota', '1G', '--writers', '2']
        if join:
            args += ['--join', self.ep(join)]
        with open(self.dir / f'n{i}.log', 'a') as log:
            self.procs[i] = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                             env=dict(env, **(extra_env or {})))
        return self.procs[i]

    def log(self, i):
        return (self.dir / f'n{i}.log').read_text(errors='replace')

    def wait_log(self, i, pattern, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.log(i)):
                return
            if self.procs[i].poll() is not None:
                break
            time.sleep(0.05)
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.log(i)[-2000:]}')

    def stop(self, i):
        p = self.procs.pop(i, None)
        if p and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()

    def stop_all(self):
        for i in list(self.procs):
            self.stop(i)

    def voters(self, i):
        rc, out = mgmt('raft-status', self.ep(i))
        assert rc == 0, out
        return {int(g): (int(h), int(v, 16)) for g, h, v in
                re.findall(r'group (\d+) hosted=(\d+) .*?voters=(0x[0-9a-f]+)', out)}

    def mkfs_until_ok(self, i, timeout=40):
        end, out = time.time() + timeout, ''
        while time.time() < end:
            rc, out = mgmt('raft-mkfs', self.ep(i))
            if rc == 0 and 'raft-mkfs rc=0 ' in out:
                return out
            time.sleep(0.5)
        raise AssertionError('raft-mkfs never succeeded: ' + out + '\n' + self.log(i)[-3000:])


def mgmt(*args):
    p = subprocess.run([str(MGMT), *args], capture_output=True, text=True, env=env, timeout=60)
    return p.returncode, p.stdout + p.stderr


t0 = time.time()
clusters = []
try:
    # 1. The removed variable stops efsd, set or empty.
    c = Cluster('refuse', 1)
    clusters.append(c)
    for value in ('3', ''):
        p = c.start(1, extra_env={'EFS_MD_RAFT_N': value})
        assert p.wait(timeout=30) == 1, c.log(1)
        assert 'EFS_MD_RAFT_N was removed' in c.log(1), c.log(1)[-800:]
        assert 'raft-host: up' not in c.log(1), c.log(1)[-800:]
        c.procs.pop(1)

    # 2. Two members: formation refused, naming the missing member.
    c = Cluster('three', 5)
    clusters.append(c)
    c.start(1)
    c.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    c.start(2, join=1)
    c.wait_log(2, r'Joined cluster with 2 nodes')
    rc, out = mgmt('raft-mkfs', c.ep(1))
    assert rc == 1 and 'missing: node 3' in out and f'node 2 at {c.ep(2)}' in out, out
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc != 0, out
    assert 'the cluster is not formed' in c.log(1), c.log(1)[-1500:]

    # 3. Third member; formation commits three members.
    c.start(3, join=1)
    c.wait_log(3, r'Joined cluster with 3 nodes')
    out = c.mkfs_until_ok(1)
    assert '3 joined members' in out, out
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members, g0 voters 0x7, g2 voters 0x7, g2 hosted')
        assert c.voters(i) == {0: (1, 7), 2: (1, 7)}, (i, c.voters(i))
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc == 0, out

    # 4. Node 5 while node 4 is absent: pending, not in the table.
    c.start(5, join=1)
    c.wait_log(5, r'Join pending at .* the cluster has 3 committed members')
    time.sleep(2.5)
    rc, out = mgmt('status', c.ep(1))
    assert 'Cluster nodes (3)' in out, out
    assert 'raft-host: formed' not in c.log(5), c.log(5)[-1500:]
    c.stop(5)

    # 5. A repeated mkfs keeps the first membership; a restart derives the
    # count and voters from the member store.
    c.mkfs_until_ok(2)
    c.stop(2)
    c.start(2, join=1)
    c.wait_log(2, r'formed \(3 committed members\)')
    c.wait_log(2, r'raft-host: up raft_id=1 n=3 formed')
    assert c.voters(2) == {0: (1, 7), 2: (1, 7)}, c.voters(2)

    # 6. A second daemon on node 3's metadata store (its data root is
    # elsewhere, so the storage-root lock does not stop it first) is
    # refused by the member store's owner lock.
    (c.dir / 'other').mkdir()
    p = subprocess.run([str(EFSD), '--node-id', '3', '--addr', HOST, '--port', str(c.ports[5]),
                        '--storage', str(c.dir / 'other'), '--meta-storage', str(c.dir / 'n3'),
                        '--quota', '1G'], capture_output=True, text=True, env=env, timeout=30)
    assert p.returncode == 1 and 'owned by another running process' in p.stderr, p.stderr[-800:]
    c.stop_all()

    # 7. Five members: the committed count selects the D38 layout.
    c = Cluster('five', 5)
    clusters.append(c)
    c.start(1)
    c.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    for i in range(2, 6):
        c.start(i, join=1)
        c.wait_log(i, r'Joined cluster with|Join pending at')
    c.mkfs_until_ok(1)
    # Node 1 keeps a retired group-2 replica when the cluster formed at
    # three and grew (K33 M6); the others are exact.
    want = {2: {0: (1, 7), 2: (1, 0xe)}, 3: {0: (1, 7), 2: (1, 0xe)},
            4: {0: (0, 7), 2: (1, 0xe)}, 5: {0: (0, 7), 2: (0, 0xe)}}
    for i in range(1, 6):
        c.wait_log(i, r'raft-host: (formed|membership revision \d+): 5 members')
    end = time.time() + 60
    while time.time() < end and any(c.voters(i) != want[i] for i in want):
        time.sleep(0.3)
    for i in want:
        assert c.voters(i) == want[i], (i, c.voters(i))
    v1 = c.voters(1)
    assert v1[0] == (1, 7), v1
finally:
    for c in clusters:
        c.stop_all()
    if os.environ.get('EFS_W153_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print('W153 formation: EFS_MD_RAFT_N refused; 2 members refused (node 3 named); 3 committed '
      '(g0/g2 voters 0x7 everywhere); metadata refused before formation; out-of-order joiner '
      'pending; repeat mkfs and restart keep the membership; store lock; 5 members '
      f'(g0 0x7, g2 0xe, node 5 hosts none): PASS ({time.time() - t0:.1f} s)')
