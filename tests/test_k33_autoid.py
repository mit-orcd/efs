#!/usr/bin/env python3
"""K33/W157 increment 2: automatic node ids, the positional CLI and discovery.

Real efsd daemons on loopback (127.0.0.0/8), private ports and stores under
TMPDIR (tests/k33_daemons.py). Discovery uses private UDP ports and probes
only `lo` (broadcast 127.255.255.255), so it never reaches another host or
the real clusters' ports. docs/status/k33-membership-protocol-20261023.md
M4' (promise formation) and M1 (discovery finds peers only).
1. No answer: a joiner without a seed says that nobody answered and waits;
   once a seed and a second joiner start on its discovery port it finds
   the forming cluster and the three form with ids 1..3 (the seed is 1,
   the joiners in endpoint order). Interface and port selection: the
   second joiner gives no --listen; it takes the address of the interface
   that reaches the seed (127.0.0.1 for a seed on 127.0.0.2) and the seed's
   service port. A third joiner that would take the same endpoint stops
   and says to give --listen.
2. Two concurrent joiners after formation get distinct ids 4 and 5.
3. Restart keeps every identity: a seed restarted before formation keeps
   its cluster identity; a joiner killed (SIGKILL) right after its promise
   keeps its id; restarted members keep id, endpoint and cluster.
4. The seed is down: a joiner started with discovery is added through
   another member that answers; one naming the down seed says the seed
   does not answer and waits, and joins once the seed is back.
5. Two clusters side by side on one discovery port stay apart: a joiner
   without a seed is refused with both listed; a joiner naming a seed
   joins that cluster only; the clusters' identities differ and neither
   lists the other's nodes.
"""
import os, re, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, free_port, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
work = Path(tempfile.mkdtemp(prefix='efs-k33id-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if k != 'EFS_TEST_MEMBER_EXIT'}
env['EFS_GC_DISABLE'] = '1'


def mgmt(*args):
    return _mgmt(env, *args)


def members(c, i):
    # status exits nonzero for a degraded cluster (a stopped member); the
    # node list is what counts here.
    rc, out = mgmt('status', c.ep(i))
    assert 'Cluster nodes (' in out, out
    return out


t0 = time.time()
clusters = []
try:
    # 1. No answer, then discovery, formation, interface and port selection.
    disc = free_port(udp=True)
    c = Cluster(work, 'disc', [1, 2, 3, 4, 5, 6], env, disc_port=disc, hosts={1: '127.0.0.2'})
    clusters.append(c)
    c.start(3, join='discover')
    c.wait_log(3, r'discovery: no EFS cluster answered on lo 127\.0\.0\.1 \(broadcast '
               r'127\.255\.255\.255\) \(UDP port %d\); waiting' % disc)
    time.sleep(3)
    assert c.alive(3), c.last_run(3)[-1500:]
    c.start(1)
    c.wait_log(1, r'discovery: answering on UDP port %d' % disc)
    # Node 2 names the seed and no endpoint: 127.0.0.1 reaches 127.0.0.2,
    # and the seed's service port is adopted.
    c.start(2, join=1, listen=False)
    c.ports[2] = c.ports[1]  # 127.0.0.1 with the seed's port (k33_daemons' host for 2)
    c.wait_log(2, r"join: this node's endpoint is 127\.0\.0\.1:%d \(the interface that reaches "
               r"127\.0\.0\.2" % c.ports[1])
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
    c.wait_log(3, r'discovery: found cluster [0-9a-f]+ \(forming')
    assert c.node_id(1) == 1, c.last_run(1)[-1500:]
    # Endpoint order: 127.0.0.1:P1 (node 2) sorts before 127.0.0.1:P3 when P1 < P3.
    want = {2: 2, 3: 3} if c.ports[1] < c.ports[3] else {2: 3, 3: 2}
    assert {i: c.node_id(i) for i in (2, 3)} == want, ({i: c.node_id(i) for i in (2, 3)}, want)
    cluster_a = c.cluster_id(1)
    assert all(c.cluster_id(i) == cluster_a for i in (2, 3))
    rc, out = mgmt('raft-getattr', c.ep(1), '1')
    assert rc == 0, out
    p = c.start(4, join=1, listen=False)
    assert p.wait(timeout=30) == 1, c.last_run(4)[-1500:]
    c.procs.pop(4)
    assert 'give another with --listen ADDR:PORT' in c.last_run(4), c.last_run(4)[-1500:]
    shutil.rmtree(c.dir / 'n4')

    # 2. Two concurrent joiners after formation: ids 4 and 5.
    c.start(4, join=1)
    c.start(5, join='discover')
    for i in (4, 5):
        c.wait_log(i, r'raft-host: up raft_id=\d n=[45] formed', timeout=60)
    assert sorted(c.node_id(i) for i in (4, 5)) == [4, 5], [c.node_id(i) for i in (4, 5)]
    for i in (1, 2, 3, 4, 5):
        c.wait_log(i, r'(membership revision 3: 5 members|formed: 5 members|n=5 formed)',
                   run=False, timeout=60)

    # 3. Restarts keep identities.
    before = {i: (c.node_id(i), c.cluster_id(i)) for i in (1, 2, 3, 4, 5)}
    for i in (1, 4):
        c.stop(i)
        if i == 1:
            c.start(1)
        else:
            c.start(4, join=1)
        c.wait_log(i, r'raft-host: up raft_id=%d n=5 formed' % (before[i][0] - 1))
        assert (c.node_id(i), c.cluster_id(i)) == before[i], (i, before[i])
    s = Cluster(work, 'restart', [1, 2, 3], env)
    clusters.append(s)
    s.start(1)
    s.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    seed_cluster = s.cluster_id(1)
    s.stop(1)
    s.start(1)
    s.wait_log(1, r'raft-host: up raft_id=0 n=3 unformed')
    assert s.cluster_id(1) == seed_cluster, (s.cluster_id(1), seed_cluster)
    assert 'initialized now' not in s.last_run(1)
    s.start(2, join=1)
    s.start(3, join=1)
    s.wait_log(3, r'Joined cluster [0-9a-f]+ as node 3 .*promised the formation proposal')
    s.stop(3, sig=signal.SIGKILL)
    s.start(3, join=1)
    s.wait_log(3, r'node 3: store .* promised \(formation proposal of 3 members\)')
    for i in (1, 2, 3):
        s.wait_log(i, r'raft-host: formed: 3 members', timeout=60, run=False)
    assert s.node_id(3) == 3 and s.cluster_id(3) == seed_cluster

    # 4. The seed is down: another member answers discovery.
    c.stop(1)
    c.start(6, join='discover')
    c.wait_log(6, r'Joined cluster %s as node 6 .*committed member: 6 members' % cluster_a,
               timeout=60)
    found = re.search(r'Joined cluster .* as node 6', c.last_run(6))
    assert found and c.alive(6)
    p = subprocess.Popen([str(EFSD), '--join', c.ep(1), '--listen', '127.0.0.1:%d' % free_port(),
                          str(c.dir / 'late'), '--quota', '1G', '--writers', '2',
                          '--discovery-port', str(disc)], stdout=open(c.dir / 'late.log', 'w'),
                         stderr=subprocess.STDOUT, env=env)
    try:
        end = time.time() + 30
        while time.time() < end and 'does not answer; retrying' not in (c.dir / 'late.log').read_text():
            time.sleep(0.1)
        late = (c.dir / 'late.log').read_text()
        assert 'join: %s does not answer; retrying' % c.ep(1) in late, late[-1500:]
        c.start(1)
        end = time.time() + 60
        while time.time() < end and 'as node 7' not in (c.dir / 'late.log').read_text():
            time.sleep(0.2)
        late = (c.dir / 'late.log').read_text()
        assert re.search(r'Joined cluster %s as node 7' % cluster_a, late), late[-2000:]
    finally:
        p.send_signal(signal.SIGTERM)
        try:
            p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            raise AssertionError('late joiner did not stop on SIGTERM:\n' +
                                 (c.dir / 'late.log').read_text()[-3000:])

    # 5. Two clusters side by side on one discovery port.
    b = Cluster(work, 'other', [1, 2, 3, 9], env, disc_port=disc)
    clusters.append(b)
    b.start(1)
    b.start(2, join=1)
    b.start(3, join=1)
    for i in (1, 2, 3):
        b.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
    cluster_b = b.cluster_id(1)
    assert cluster_b != cluster_a
    p = b.start(9, join='discover')
    assert p.wait(timeout=30) == 1, b.last_run(9)[-2000:]
    b.procs.pop(9)
    out = b.last_run(9)
    assert re.search(r'discovery: 2 clusters answer', out) and cluster_a in out and \
        cluster_b in out and 'several clusters answer' in out, out[-2000:]
    shutil.rmtree(b.dir / 'n9')
    b.start(9, join=2)
    b.wait_log(9, r'Joined cluster %s as node 4' % cluster_b, timeout=60)
    assert 'Cluster nodes (4)' in members(b, 2), members(b, 2)
    out = members(c, 2)
    assert 'Cluster nodes (7)' in out and b.ep(9) not in out, out
finally:
    for c in clusters:
        c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print('K33 automatic ids: no answer said and waited, then discovered; 3 formed with ids 1..3 '
      '(endpoint order) and the adopted interface/port; endpoint clash says --listen; two '
      'concurrent joiners got 4 and 5; restarts kept seed cluster, promised id and member ids; '
      'seed down: added through another member, a joiner naming the seed waited for it; two '
      f'clusters apart by identity: PASS ({time.time() - t0:.1f} s)')
