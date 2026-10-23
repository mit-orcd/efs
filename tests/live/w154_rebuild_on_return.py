#!/usr/bin/env python3
"""W154 live gate: rebuild on return of stripes acknowledged with two fragments.

    w154_rebuild_on_return.py REL WORK [PORT]

A private five-daemon TCP cluster on loopback (ports PORT+1..PORT+5, default
PORT 19100), five configured members in WORK/cluster.conf (D39), stores
WORK/n1..n5, one efs-fuse mount at
WORK/mnt. REL holds efsd, efs-fuse, efs-mgmt and scripts/ of one build.
WORK must be absolute. Stops only the PIDs it started and unmounts only
WORK/mnt. The F1 scenario (gates-xorinox-20261022.md):

  1. baseline files with all nodes up;
  2. node 5 stops (SIGTERM): files written in the window before the client
     marks it down, and again after the client's 30 s down cooldown, get
     degraded acknowledgements; efs-mgmt status shows the count, which
     holds while node 5 is away;
  3. node 5 restarts; with the rebuild paced (EFS_REBUILD_PER_PASS=4) two
     degraded files are rewritten while the rebuild runs (a rewrite retires
     the mark instead of racing the rebuild);
  4. the count returns to zero, every chunk of every file has its three
     fragments on three distinct nodes in the stores, and every rebuild is
     logged;
  5. node 1 (a different node, the seed and a group leader) stops, and every
     file reads back correctly from a cold remount.

Prints one PASS line per check and exits nonzero on the first failure. The
daemon and client logs stay under WORK/logs for inspection.
"""
import hashlib
import os
import random
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

REL = Path(sys.argv[1]).resolve()
WORK = Path(sys.argv[2])
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 19100
assert WORK.is_absolute(), 'WORK must be absolute (client.sh stop matches it)'
N = 5
ADDR = '127.0.0.1'
MNT = WORK / 'mnt'
LOGS = WORK / 'logs'
MIB = 1 << 20
# D39: the configured member list; efs-mgmt declares as management with it.
CONF = WORK / 'cluster.conf'
ENV = dict(os.environ, EFS_TRANSPORT='tcp', EFS_CLUSTER_CONF=str(CONF),
           EFS_REBUILD_PER_PASS='4')
procs = {}
manifest = {}


def ep(i):
    return f'{ADDR}:{PORT + i}'


def mgmt(*args, check=True):
    r = subprocess.run([str(REL / 'efs-mgmt'), *args], capture_output=True, text=True,
                       env=ENV, timeout=60)
    if check and r.returncode not in (0, 1):
        raise AssertionError(f'efs-mgmt {args}: rc={r.returncode} {r.stderr}')
    return r.stdout


def listening(port):
    r = subprocess.run(['ss', '-ltnH', f'sport = :{port}'], capture_output=True, text=True)
    return bool(r.stdout.strip())


def start_node(i):
    assert not listening(PORT + i), f'port {PORT + i} in use'
    if not CONF.exists():
        CONF.write_text(''.join(f'{n} {ep(n)}\n' for n in range(1, N + 1)))
    cmd = [str(REL / 'efsd'), '--cluster', str(CONF), '--addr', ADDR, '--port', str(PORT + i),
           '--storage', str(WORK / f'n{i}'), '--quota', '4G', '--no-direct-io']
    log = open(LOGS / f'efsd-n{i}.log', 'a')
    procs[i] = subprocess.Popen(cmd, env=ENV, stdout=log, stderr=log)
    for _ in range(240):
        assert procs[i].poll() is None, f'node {i} exited'
        if listening(PORT + i):
            return
        time.sleep(0.25)
    raise AssertionError(f'node {i} not listening')


def stop_node(i, sig=signal.SIGTERM):
    p = procs.pop(i, None)
    if not p:
        return
    p.send_signal(sig)  # SIGKILL also ends a stopped process, without running it
    try:
        p.wait(20)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def mount(via):
    MNT.mkdir(exist_ok=True)
    env = dict(ENV, EFS_FUSE_LOG=str(LOGS / 'fuse.log'))
    r = subprocess.run([str(REL / 'scripts/client.sh'), ep(via), str(MNT)], env=env,
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and subprocess.run(['mountpoint', '-q', str(MNT)]).returncode == 0, \
        ('mount failed', r.stdout[-500:], r.stderr[-500:])


def unmount():
    if subprocess.run(['mountpoint', '-q', str(MNT)]).returncode == 0:
        r = subprocess.run([str(REL / 'scripts/client.sh'), 'stop', str(MNT)], env=ENV,
                           capture_output=True, text=True, timeout=180)
        assert r.returncode == 0, ('client stop failed', r.stdout[-500:], r.stderr[-500:])


def write_file(name, mib, seed):
    rnd = random.Random(seed)
    h = hashlib.sha256()
    path = MNT / name
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'wb') as f:
        for _ in range(mib):
            block = rnd.randbytes(MIB)
            h.update(block)
            f.write(block)
    # close() publishes. fsync would also COMMIT every storage node, which
    # answers EIO while one is down (by design), so it is not used here.
    manifest[name] = h.hexdigest()


def verify_all(label):
    bad = []
    for name, want in sorted(manifest.items()):
        h = hashlib.sha256()
        try:
            with open(MNT / name, 'rb') as f:
                for block in iter(lambda: f.read(MIB), b''):
                    h.update(block)
        except OSError as e:
            bad.append((name, str(e)))
            continue
        if h.hexdigest() != want:
            bad.append((name, 'mismatch'))
    assert not bad, (label, bad)
    print(f'PASS {label}: {len(manifest)} files read back correctly', flush=True)


def status():
    return mgmt('status', ep(next(i for i in range(1, N + 1) if i in procs)))


def degraded():
    """(count or None, status text): None until both group leaders report."""
    out = status()
    m = re.search(r'^Degraded chunks: (\d+) ', out, re.M)
    return (int(m.group(1)) if m else None), out


def wait_degraded(pred, timeout, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        n, out = degraded()
        if n is not None and pred(n):
            return n, out
        time.sleep(1)
    raise AssertionError((label, degraded()[1]))


def chunk_map(ino):
    """ci -> (nodes, obj_gen), merged over the group leaders' answers."""
    rows = {}
    env = dict(ENV, EFS_MGMT_CHUNKS='100000')
    for i in sorted(procs):
        start = 0
        while True:
            r = subprocess.run([str(REL / 'efs-mgmt'), 'raft-getchunks', ep(i), str(ino),
                                str(start)], capture_output=True, text=True, env=env, timeout=60)
            m = re.search(r'raft-getchunks status=(\d+) primary=\d+ ino=\d+ count=(\d+)', r.stdout)
            if not m or m.group(1) != '0' or m.group(2) == '0':
                break
            got = 0
            for c in re.finditer(r'ci=(\d+) nodes=(\d+),(\d+),(\d+) base_gen=\d+ obj_gen=(\d+)',
                                 r.stdout):
                ci = int(c.group(1))
                rows[ci] = ((int(c.group(2)), int(c.group(3)), int(c.group(4))), int(c.group(5)))
                start = max(start, ci + 1)
                got += 1
            if not got:
                break
    return rows


def store_index():
    """node -> set of fragment basenames '{ci}.{fi}.{gen}' in its store."""
    idx = {}
    for i in range(1, N + 1):
        names = set()
        for root, _dirs, files in os.walk(WORK / f'n{i}' / 'data' / 'exports'):
            names.update(files)
        idx[i] = names
    return idx


def main():
    for i in range(1, N + 1):
        (WORK / f'n{i}').mkdir(parents=True, exist_ok=True)
    LOGS.mkdir(parents=True, exist_ok=True)
    try:
        for i in range(1, N + 1):
            start_node(i)
        for _ in range(60):
            if 'raft-mkfs rc=0 ' in mgmt('raft-mkfs', ep(1), check=False):
                break
            time.sleep(1)
        else:
            raise AssertionError('raft-mkfs')
        mount(1)
        n, out = wait_degraded(lambda n: n == 0, 60, 'leaders report')
        assert 'Cluster state: OK (5 up' in out, out
        print('PASS cluster up: 5 nodes, both group leaders report 0 degraded chunks', flush=True)

        for k in range(4):
            write_file(f'base/f{k}', 16, 100 + k)
        verify_all('baseline')
        assert degraded()[0] == 0, 'full stripes must not be marked'
        print('PASS full stripes leave no degraded mark', flush=True)

        # Node 5 (it hosts no metadata group) stops. Until the client marks
        # it down (four failures, then 30 s), chunks with a fragment for it
        # are acknowledged by the other two: write at once, and again when
        # the client's down cooldown has run out.
        stop_node(5)  # clean stop; a crash restart fences COMMIT (see the page)
        for k in range(4):
            write_file(f'hung/f{k}', 16, 200 + k)
        time.sleep(32)
        for k in range(2):
            write_file(f'down/f{k}', 16, 300 + k)
        write_file('base/f0', 16, 400)  # rewrite a full-stripe file while degraded
        verify_all('written with node 5 away')
        acks = sum(1 for line in open(LOGS / 'fuse.log', errors='replace')
                   if 'degraded acknowledgement, fragment' in line)
        suppressed = sum(int(m.group(1)) for m in re.finditer(
            r'(\d+) more degraded acknowledgements', open(LOGS / 'fuse.log', errors='replace').read()))
        assert acks > 0, 'no degraded acknowledgement in the window'
        # Publication after close() is asynchronous: every acknowledged
        # object (each written once here) becomes exactly one mark.
        n_marks, out = wait_degraded(lambda n: n == acks + suppressed, 120,
                                     'one mark per degraded acknowledgement')
        print(f'PASS degraded acknowledgements: {n_marks} marks, {acks} log lines '
              f'(+{suppressed} summarized); status: '
              f'{re.search(r"^Degraded chunks: .*$", out, re.M).group(0)}', flush=True)
        time.sleep(5)
        held = degraded()[0]
        assert held == n_marks, ('marks must hold while node 5 is away', n_marks, held)
        print(f'PASS marks held while node 5 is down ({held})', flush=True)

        # At-risk chunks: degraded ones whose surviving pair includes node 1.
        inos = {name: os.stat(MNT / name).st_ino for name in manifest}
        maps = {name: chunk_map(ino) for name, ino in inos.items()}
        idx = store_index()
        at_risk = 0
        for name, rows in maps.items():
            for ci, (nodes, gen) in rows.items():
                have = [fi for fi in range(3) if f'{ci}.{fi}.{gen}' in idx[nodes[fi]]]
                if len(have) == 2 and 1 in [nodes[fi] for fi in have]:
                    at_risk += 1
        assert at_risk > 0, 'no degraded chunk depends on node 1: the F1 check would prove nothing'
        print(f'PASS {at_risk} degraded chunks would lose a second fragment with node 1', flush=True)

        # Return of node 5, paced rebuild, and two rewrites racing it.
        start_node(5)
        write_file('hung/f0', 16, 500)
        write_file('down/f1', 16, 501)
        n, out = wait_degraded(lambda n: n == 0, 300, 'rebuild drains the marks')
        rebuilt = sum(len(re.findall(r'raft-host: rebuilt ino=', open(LOGS / f'efsd-n{i}.log',
                                                                       errors='replace').read()))
                      for i in range(1, N + 1))
        m = re.search(r'rebuilt=(\d+) retired=(\d+) failures=(\d+)', out)
        assert rebuilt > 0 and m and int(m.group(1)) == rebuilt, (rebuilt, out)
        assert rebuilt < n_marks, ('the rewrites should have retired some marks', rebuilt, n_marks)
        print(f'PASS rebuild on return: 0 degraded chunks, {rebuilt} fragments rebuilt and '
              f'logged, {n_marks - rebuilt} marks retired by the racing rewrites '
              f'(status: {m.group(0)})', flush=True)
        verify_all('after rebuild')

        idx = store_index()
        chunks = 0
        for name in manifest:
            rows = chunk_map(os.stat(MNT / name).st_ino)
            assert len(rows) == 16 * MIB // (128 * 1024), (name, len(rows))
            for ci, (nodes, gen) in rows.items():
                assert len(set(nodes)) == 3, (name, ci, nodes)
                for fi in range(3):
                    assert f'{ci}.{fi}.{gen}' in idx[nodes[fi]], (name, ci, fi, nodes, gen)
                chunks += 1
        print(f'PASS {chunks} chunks of {len(manifest)} files have three fragments on '
              'three distinct nodes', flush=True)

        # F1: a different single-node outage after the return.
        unmount()
        stop_node(1)
        mount(2)
        verify_all('node 1 down, cold remount')
        unmount()
        start_node(1)
        print('PASS W154 rebuild on return', flush=True)
    finally:
        try:
            unmount()
        except Exception as e:  # keep stopping daemons
            print('unmount:', e, flush=True)
        for i in list(procs):
            stop_node(i)


main()
