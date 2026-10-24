#!/usr/bin/env python3
"""K33/W157: automatic formation and online node addition, real daemons.

Real efsd daemons on loopback with private stores under TMPDIR and free
ports (never 17xxx); docs/status/k33-membership-protocol-20261023.md M1-M9.
1. One and two nodes stay unready: they log why, no metadata is served,
   nothing is formed.
2. A third member joins: the cluster forms by itself (no efs-mgmt
   raft-mkfs); group 0 and group 2 vote on {1,2,3}; metadata is served.
   A FUSE client writes files (when FUSE is available).
3. Node 4 joins through node 2, which ends right after committing it
   (EFS_TEST_MEMBER_EXIT=after-members-commit): the group-0 leader's notice
   pump still installs revision 2 on node 4; node 2 restarts and learns it
   from its own replica. Group 2 moves {1,2,3} -> {2,3,4} by joint
   consensus; node 4 hosts it.
4. Node 5 joins and ends when its notice arrives, before persisting
   (EFS_TEST_MEMBER_EXIT=before-learn); restarted, it is the committed
   member and learns revision 3. Node 5 hosts no metadata group.
5. The seed (node 1) stops. Node 7 joins first and waits (pending: id 6
   is next); two stores claim id 6 at once, through nodes 2 and 4:
   exactly one commits, the other is
   refused and exits; then node 7 is added through node 5, which forwards
   it (no group-0 replica). Node 1 restarts and learns revisions 4 and 5.
6. A member restarted with a wiped store (new token) is refused and exits;
   it stays a member. Orderly restarts keep every count and voter set.
7. The files written in step 2 read back exactly through a new mount, and
   new files place fragments on the added nodes (usable capacity).
"""
import hashlib, os, re, shutil, signal, socket, subprocess, sys, tempfile, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
EFSD, MGMT, FUSE = root / 'efsd', root / 'efs-mgmt', root / 'efs-fuse'
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
HOST = '127.0.0.1'
work = Path(tempfile.mkdtemp(prefix='efs-k33-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
env.pop('EFS_TEST_MEMBER_EXIT', None)
env['EFS_GC_DISABLE'] = '1'


def free_port():
    with socket.socket() as s:
        s.bind((HOST, 0))
        return s.getsockname()[1]


def mgmt(*args):
    p = subprocess.run([str(MGMT), *args], capture_output=True, text=True, env=env, timeout=60)
    return p.returncode, p.stdout + p.stderr


class Cluster:
    def __init__(self, name, ids):
        self.dir = work / name
        self.dir.mkdir()
        self.ports = {i: free_port() for i in ids}
        self.procs = {}

    def ep(self, i):
        return f'{HOST}:{self.ports[i]}'

    def start(self, i, join=None, store=None, extra_env=None, node_id=None):
        store = self.dir / (store or f'n{i}')
        store.mkdir(exist_ok=True)
        args = [str(EFSD), '--node-id', str(node_id or i), '--addr', HOST, '--port',
                str(self.ports[i]), '--storage', str(store), '--quota', '1G', '--writers', '2']
        if join:
            args += ['--join', self.ep(join)]
        with open(self.dir / f'n{i}.log', 'a') as log:
            log.write(f'\n==== start {time.time():.3f} {" ".join(args)}\n')
            log.flush()
            self.procs[i] = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT,
                                             env=dict(env, **(extra_env or {})))
        return self.procs[i]

    def log(self, i):
        return (self.dir / f'n{i}.log').read_text(errors='replace')

    def last_run(self, i):
        return self.log(i).rsplit('==== start', 1)[-1]

    def wait_log(self, i, pattern, timeout=30, run=True):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.last_run(i) if run else self.log(i)):
                return
            if i in self.procs and self.procs[i].poll() is not None and run:
                if re.search(pattern, self.last_run(i)):
                    return
                break
            time.sleep(0.05)
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.last_run(i)[-3000:]}')

    def wait_exit(self, i, timeout=30):
        rc = self.procs[i].wait(timeout=timeout)
        self.procs.pop(i)
        return rc

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

    def status(self, i):
        out = ''
        for _ in range(3):
            rc, out = mgmt('raft-status', self.ep(i))
            if rc == 0:
                # (serves, voters): a retired replica (K33 M6) does not serve.
                return {int(g): (int(h) and not int(r), int(v, 16)) for g, h, v, r in
                        re.findall(r'group (\d+) hosted=(\d+) .*?voters=(0x[0-9a-f]+) joint=\d+ '
                                   r'retired=(\d+)', out)}
            time.sleep(0.5)
        raise AssertionError(out)

    def wait_voters(self, ids, want, timeout=60):
        end, got = time.time() + timeout, {}
        while time.time() < end:
            got = {i: self.status(i) for i in ids}
            if all(want(i, got[i]) for i in ids):
                return got
            time.sleep(0.3)
        raise AssertionError(f'voters never settled: {got}\n' +
                             '\n'.join(self.last_run(i)[-1500:] for i in ids))


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


def payload(name, size):
    out, k = b'', 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return out[:size]


def fragments(store):
    """Fragment files under one node's storage root (data plane, not metadata)."""
    n = 0
    for dirpath, dirnames, files in os.walk(store):
        if '/mdraft' in dirpath or dirpath.endswith('/meta'):
            continue
        n += sum(1 for f in files if re.match(r'^\d+[._-]', f) or f.endswith('.frag'))
    return n


t0 = time.time()
c = None
mounts = []
notes = []
try:
    c = Cluster('grow', [1, 2, 3, 4, 5, 6, 7, 8])

    # 1. One and two nodes stay unready.
    c.start(1)
    c.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    c.wait_log(1, r'raft-host: unready: the cluster is not formed; formation needs at least 3')
    c.start(2, join=1, extra_env={'EFS_TEST_MEMBER_EXIT': 'after-members-commit'})
    c.wait_log(2, r'Joined cluster with 2 nodes')
    time.sleep(2.5)
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc != 0, out
    for i in (1, 2):
        assert 'raft-host: formed' not in c.log(i), c.log(i)[-2000:]
    assert 'the cluster is not formed' in c.log(1), c.log(1)[-1500:]

    # 2. The third member: formation without any operator step.
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members, g0 voters 0x7, g2 voters 0x7, g2 hosted')
    assert 'forming automatically with 3 members' in c.log(1) + c.log(2) + c.log(3)
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc == 0, out
    files = {}
    if fuse_ok():
        m = Mount(c, 1, 'mnt-a')
        mounts.append(m)
        for k, size in enumerate((0, 1, 4096, 100000, 3 * 1024 * 1024 + 17)):
            name = f'before-{k}'
            (m.path / name).write_bytes(payload(name, size))
            files[name] = size
        os.sync()
    else:
        notes.append('NOTRUN FUSE payload checks (no /dev/fuse, fusermount3 or efs-fuse)')

    # 3. Node 4 through node 2; node 2 ends right after the commit.
    c.start(4, join=2)
    c.wait_log(2, r'membership: committed node 4 .* as member 4 \(revision 2\)', run=False)
    c.wait_log(2, r'EFS_TEST_MEMBER_EXIT at after-members-commit', run=False)
    assert c.wait_exit(2) == 86
    c.wait_log(4, r'raft-host: formed: 4 members')
    for i in (1, 3):
        c.wait_log(i, r'membership revision 2: 4 members')
    c.start(2, join=1)
    c.wait_log(2, r'raft-host: up raft_id=1 n=4 formed')
    want4 = {1: lambda v: v[0] == (1, 7) and v[2][0] == 0,
             2: lambda v: v == {0: (1, 7), 2: (1, 0xe)},
             3: lambda v: v == {0: (1, 7), 2: (1, 0xe)},
             4: lambda v: v[2] == (1, 0xe) and v[0][0] == 0}
    c.wait_voters([1, 2, 3, 4], lambda i, v: want4[i](v))
    assert re.search(r'membership: group 2 voters 0x7 -> 0xe: joint consensus started',
                     ''.join(c.log(i) for i in (1, 2, 3))), 'no joint change logged'

    # 4. Node 5 ends when its notice arrives; restarted it learns.
    c.start(5, join=3, extra_env={'EFS_TEST_MEMBER_EXIT': 'before-learn'})
    c.wait_log(5, r'EFS_TEST_MEMBER_EXIT at before-learn')
    assert c.wait_exit(5) == 86
    assert 'raft-host: formed' not in c.log(5)
    c.start(5, join=3)
    c.wait_log(5, r'raft-host: formed: 5 members')
    for i in (1, 2, 3, 4):
        c.wait_log(i, r'membership revision 3: 5 members')
    v5 = c.status(5)
    assert v5[0][0] == 0 and v5[2][0] == 0, v5

    # 5. Seed loss; node 7 waits for id 6; two stores claim id 6.
    c.stop(1)
    c.start(7, join=5)  # node 5 hosts no group-0 replica: it forwards
    c.wait_log(7, r'Join pending at .* the cluster has 5 committed members')
    c.start(6, join=2)
    c.start(8, join=4, node_id=6, store='n6-rival')
    end = time.time() + 60
    while time.time() < end and not (6 in c.procs and 8 in c.procs and
                                     ('raft-host: formed' in c.last_run(6) or
                                      'raft-host: formed' in c.last_run(8))):
        time.sleep(0.1)
    winner = 6 if 'raft-host: formed' in c.last_run(6) else 8
    loser = 14 - winner
    assert c.wait_exit(loser, timeout=60) == 1, c.last_run(loser)[-2000:]
    assert 'can never be a member of this cluster' in c.last_run(loser), c.last_run(loser)[-2000:]
    c.wait_log(7, r'raft-host: formed: 7 members', timeout=60)
    c.wait_log(5, r'membership: forwarded node 7 at .* to member [23]: rc=0', run=True)
    for i in (2, 3, 4, 5, winner):
        c.wait_log(i, r'membership revision 5: 7 members', timeout=60, run=False)
    c.start(1)
    c.wait_log(1, r'raft-host: (up raft_id=0 n=7 formed|membership revision 5: 7 members)', timeout=60)
    for i in (2, 3, 4, 5, 7, winner):
        v = c.status(i)
        assert v[0][1] == 7 and v[2][1] == 0xe, (i, v)
        assert (v[0][0] == 1) == (i in (2, 3)) and (v[2][0] == 1) == (i in (2, 3, 4)), (i, v)
    rc, out = mgmt('status', c.ep(3))
    assert rc == 0 and 'Cluster nodes (7)' in out, out

    # 6. A wiped store at a member's id is refused; restarts keep the state.
    c.stop(5)
    shutil.rmtree(c.dir / 'n5')
    c.start(5, join=3)
    assert c.wait_exit(5) == 1, c.last_run(5)[-2000:]
    assert 'can never be a member of this cluster' in c.last_run(5), c.last_run(5)[-2000:]
    c.stop(4)
    c.start(4, join=2)
    c.wait_log(4, r'raft-host: up raft_id=3 n=7 formed')
    c.wait_voters([2, 3, 4], lambda i, v: v[2] == (1, 0xe))

    # 7. Old payloads read back; new writes use the added nodes.
    if files:
        m = Mount(c, 3, 'mnt-b')
        mounts.append(m)
        for name, size in files.items():
            assert (m.path / name).read_bytes() == payload(name, size), name
        for k in range(24):
            name = f'after-{k}'
            (m.path / name).write_bytes(payload(name, 256 * 1024))
        os.sync()
        for k in range(24):
            name = f'after-{k}'
            assert (m.path / name).read_bytes() == payload(name, 256 * 1024), name
        for name, size in files.items():
            assert (mounts[0].path / name).read_bytes() == payload(name, size), name
        added = {i: fragments(c.dir / ('n6-rival' if (i == 6 and winner == 8) else f'n{i}'))
                 for i in (4, 6, 7)}
        assert sum(added.values()) > 0, added
        notes.append(f'fragments on added nodes {added}')
finally:
    for m in mounts:
        m.close()
    if c:
        c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
for n in notes:
    print(n)
print('K33 growth: 1-2 nodes unready; 3 formed automatically; node 4 added across a proposer '
      'exit (g2 {1,2,3}->{2,3,4} joint); node 5 across a joiner exit (storage only); seed down, '
      'pending id 7, two claims of id 6 (one committed, one refused), node 7 added; wiped store '
      f'refused; restarts keep counts and voters: PASS ({time.time() - t0:.1f} s)')
