#!/usr/bin/env python3
"""K33/W157: automatic formation and online node addition, real daemons.

Real efsd daemons on loopback with private stores under TMPDIR and free
ports (never 17xxx), the positional CLI without node ids
(tests/k33_daemons.py); docs/status/k33-membership-protocol-20261023.md
M1-M9.
1. One and two nodes stay unready: they log why, no metadata is served,
   nothing is formed.
2. A third node joins: the cluster forms by itself (no efs-mgmt
   raft-mkfs); group 0 and group 2 vote on {1,2,3}; metadata is served.
   A FUSE client writes files (when FUSE is available).
3. Node 4 joins through node 2, which ends right after committing it
   (EFS_FAULT_MEMBER_EXIT=after-members-commit): the joiner asks another
   member the answer named and learns revision 2; node 2 restarts and
   learns it from its own replica. Group 2 moves {1,2,3} -> {2,3,4} by
   joint consensus; node 4 hosts it.
4. Node 5 ends when the committed record naming it arrives, before
   persisting it (EFS_FAULT_MEMBER_EXIT=before-learn); restarted (a retried
   join), it is the same committed member 5. Node 5 hosts no metadata group.
5. The seed (node 1) stops. Three joining stores start at once, through
   nodes 2, 4 and 5 (node 5 has no group-0 replica and forwards): each gets
   its own id, 6, 7 and 8 in commit order. Node 1 restarts and learns
   revisions 4 to 6.
6. A wiped store at a member's endpoint (new token) is refused and exits;
   the member stays a member. Orderly restarts keep every count and voter set.
7. The files written in step 2 read back exactly through a new mount, and
   new files place fragments on every added node (usable capacity).
"""
import hashlib, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster as _Cluster, EFSD, MGMT, FUSE, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-k33-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
env.pop('EFS_FAULT_MEMBER_EXIT', None)
env['EFS_FAULT_GC_DISABLE'] = '1'


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
    c.wait_log(1, r'raft-host: unready: formation needs 3 members')
    c.start(2, join=1, extra_env={'EFS_FAULT_MEMBER_EXIT': 'after-members-commit'})
    c.wait_log(2, r'join: waiting at .* is forming \(2 joined; formation needs 3\)')
    time.sleep(2.5)
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc != 0, out
    for i in (1, 2):
        assert 'raft-host: formed' not in c.log(i), c.log(i)[-2000:]
    assert 'formation needs 3 members' in c.log(1), c.log(1)[-1500:]

    # 2. The third node: formation without any operator step.
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members, g0 voters 0x7, g2 voters 0x7, g2 hosted')
        assert c.node_id(i) == i, i
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
    c.wait_log(2, r'membership: committed .* as member 4 \(revision 2\)', run=False)
    c.wait_log(2, r'EFS_FAULT_MEMBER_EXIT at after-members-commit', run=False)
    assert c.wait_exit(2) == 86
    c.wait_log(4, r'Joined cluster [0-9a-f]+ as node 4 .*committed member: 4 members')
    c.wait_log(4, r'raft-host: up raft_id=3 n=4 formed')
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

    # K44/W167: the seed retains a retired g2 log after this addition.
    # Its SALT must use the live leader's verdict, never that frozen cursor.
    rc, out = mgmt('raft-mkfs', c.ep(1))
    assert rc == 0, 'K44 retired seed: ' + out
    rc, out = mgmt('commands-policy', c.ep(1), 'get')
    assert rc == 0, out
    version = re.search(r'version=(\d+)', out).group(1)
    mask = re.search(r'mask=(\d+)', out).group(1)
    rc, out = mgmt('commands-policy', c.ep(1), 'set', mask, version)
    assert rc == 0, out
    rc, out = mgmt('raft-resync', c.ep(1), '2')
    assert rc != 0 and 'rc=-15' in out, out
    if mounts:
        name = 'after-retirement'
        files[name] = 4097
        with (mounts[0].path / name).open('wb') as f:
            f.write(payload(name, files[name]))
            f.flush()
            os.fsync(f.fileno())
        assert (mounts[0].path / name).read_bytes() == payload(name, files[name])

    # 4. Node 5 ends when the record naming it arrives; restarted (a
    # retried join with the same store) it is the same member.
    c.start(5, join=3, extra_env={'EFS_FAULT_MEMBER_EXIT': 'before-learn'})
    c.wait_log(5, r'EFS_FAULT_MEMBER_EXIT at before-learn')
    assert c.wait_exit(5) == 86
    assert 'Joined cluster' not in c.log(5)
    c.wait_log(3, r'membership: committed .* as member 5 \(revision 3\)', run=False)
    c.start(5, join=3)
    c.wait_log(5, r'Joined cluster [0-9a-f]+ as node 5 .*committed member: 5 members')
    c.wait_log(5, r'raft-host: up raft_id=4 n=5 formed')
    for i in (1, 2, 3, 4):
        c.wait_log(i, r'membership revision 3: 5 members', run=False)
    v5 = c.status(5)
    assert v5[0][0] == 0 and v5[2][0] == 0, v5

    # 5. Seed loss; three joining stores at once: distinct ids by commit order.
    c.stop(1)
    c.start(6, join=2)
    c.start(7, join=4)
    c.start(8, join=5)  # node 5 hosts no group-0 replica: it forwards
    for i in (6, 7, 8):
        c.wait_log(i, r'raft-host: up raft_id=\d+ n=[678] formed', timeout=90)
    ids = {i: c.node_id(i) for i in (6, 7, 8)}
    assert sorted(ids.values()) == [6, 7, 8], ids
    c.wait_log(5, r'membership: forwarded 127\.0\.0\.1:%d to member [23]: rc=0' % c.ports[8],
               run=False)
    for i in (2, 3, 4, 5, 6, 7, 8):
        c.wait_log(i, r'(membership revision 6: 8 members|n=8 formed)', timeout=60, run=False)
    c.start(1)
    c.wait_log(1, r'raft-host: (up raft_id=0 n=8 formed|membership revision 6: 8 members)',
               timeout=60)
    for i in (2, 3, 4, 5, 6, 7, 8):
        nid = c.node_id(i)
        v = c.status(i)
        assert v[0][1] == 7 and v[2][1] == 0xe, (i, v)
        assert (v[0][0] == 1) == (nid in (2, 3)) and (v[2][0] == 1) == (nid in (2, 3, 4)), (i, v)
    rc, out = mgmt('status', c.ep(3))
    assert rc == 0 and 'Cluster nodes (8)' in out, out
    # A9/D42: elections alone place the leaders (no balance pump); status
    # reports where they are. Each group's leader is one of its voters.
    led = c.leader_placement(3)
    assert sorted(led) == [0, 2] and led[0] in (1, 2, 3) and led[2] in (2, 3, 4), led
    print('leader placement at 8 members: group 0 -> node %d, group 2 -> node %d (%s)'
          % (led[0], led[2], 'colocated' if led[0] == led[2] else 'spread'))

    # 6. A wiped store at a member's endpoint is refused; restarts keep the state.
    c.stop(5)
    shutil.rmtree(c.dir / 'n5')
    c.start(5, join=3)
    assert c.wait_exit(5) == 1, c.last_run(5)[-2000:]
    assert 'can never be a member' in c.last_run(5), c.last_run(5)[-2000:]
    c.stop(4)
    c.start(4, join=2)
    c.wait_log(4, r'raft-host: up raft_id=3 n=8 formed')
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
        added = {i: fragments(c.dir / f'n{i}') for i in (4, 6, 7, 8)}
        # Every added node takes new fragments: 48 new stripes over eight
        # members (node 5 down, its share goes to hashed spares). The old
        # FNV start left nodes 3 and 4 with none (K33 increment 2 finding).
        assert all(v > 0 for v in added.values()), added
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
print('K33 growth: 1-2 nodes unready; 3 formed automatically with ids 1..3; node 4 added across '
      'a proposer exit (g2 {1,2,3}->{2,3,4} joint); node 5 across a joiner exit before learning '
      '(same id on retry; storage only); seed down, three concurrent joiners get ids 6, 7, 8 '
      '(one forwarded); wiped store refused; restarts keep counts and voters: '
      f'PASS ({time.time() - t0:.1f} s)')
