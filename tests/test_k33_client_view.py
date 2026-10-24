#!/usr/bin/env python3
"""K33 live client view: mounted FUSE clients use added members without a remount.

Real efsd daemons on loopback with private stores under TMPDIR and free
ports (never 17xxx); docs/status/k33-membership-protocol-20261023.md M7.
1. Three nodes form; mount A (through node 1) and mount B (through node 3)
   start at revision 1 with three members. Files written before any
   addition.
2. A writer keeps writing and fsyncing 1 MiB files through mount A for the
   whole run.
3. Node 4 joins: both mounts log the installed revision 2 ("added member 4,
   no remount"). New files written through A after the install have chunks
   whose recorded fragment nodes include node 4, and node 4's store holds
   fragments.
4. Node 5 joins: revision 3 is installed the same way; new files have
   chunks on node 5.
5. Mount B reads every file A wrote (old placement on 1..3, new placement on
   4 and 5: B connects to them on demand) and A reads them back too; all are
   exact. Neither mount restarted, remounted or logged a refused record:
   same process, still mounted, one discovery and one mount-time record each.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import hashlib, os, re, shutil, signal, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
EFSD, MGMT, FUSE = root / 'efsd', root / 'efs-mgmt', root / 'efs-fuse'
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not (FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and shutil.which('fusermount3')):
    print('K33 client view: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
HOST = '127.0.0.1'
MiB = 1024 * 1024
work = Path(tempfile.mkdtemp(prefix='efs-k33cv-', dir=os.environ.get('TMPDIR')))
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
    def __init__(self, ids):
        self.dir = work
        self.ports = {i: free_port() for i in ids}
        self.procs = {}

    def ep(self, i):
        return f'{HOST}:{self.ports[i]}'

    def start(self, i, join=None):
        store = self.dir / f'n{i}'
        store.mkdir(exist_ok=True)
        args = [str(EFSD), '--node-id', str(i), '--addr', HOST, '--port', str(self.ports[i]),
                '--storage', str(store), '--quota', '2G', '--writers', '2']
        if join:
            args += ['--join', self.ep(join)]
        with open(self.dir / f'n{i}.log', 'a') as log:
            self.procs[i] = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, env=env)

    def log(self, i):
        return (self.dir / f'n{i}.log').read_text(errors='replace')

    def wait_log(self, i, pattern, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.log(i)):
                return
            assert self.procs[i].poll() is None, f'node {i} exited\n{self.log(i)[-3000:]}'
            time.sleep(0.05)
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.log(i)[-3000:]}')

    def stop_all(self):
        for p in self.procs.values():
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in self.procs.values():
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()


class Mount:
    def __init__(self, c, node, name):
        self.name = name
        self.path = c.dir / name
        self.path.mkdir()
        self.logp = c.dir / f'{name}.log'
        self.logf = open(self.logp, 'w')
        self.p = subprocess.Popen([str(FUSE), c.ep(node), str(self.path), '-f'],
                                  stdout=self.logf, stderr=subprocess.STDOUT, env=env)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(self.path):
            assert self.p.poll() is None, self.log()[-2000:]
            time.sleep(0.1)
        assert os.path.ismount(self.path), self.log()[-2000:]

    def log(self):
        self.logf.flush()
        return self.logp.read_text(errors='replace')

    def wait_log(self, pattern, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.log()):
                return
            time.sleep(0.05)
        raise AssertionError(f'{self.name}: {pattern!r} not seen\n{self.log()[-3000:]}')

    def close(self):
        subprocess.run(['fusermount3', '-u', str(self.path)], capture_output=True)
        try:
            self.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def payload(name, size):
    seed = hashlib.sha256(name.encode()).digest()
    block = b''.join(hashlib.sha256(seed + k.to_bytes(4, 'big')).digest() for k in range(2048))
    return (block * (size // len(block) + 1))[:size]


def write(m, name, size):
    data = payload(name, size)
    with open(m.path / name, 'wb') as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    return size


def chunk_nodes(c, m, name):
    """Recorded fragment nodes of every chunk of one file (efs-mgmt raft-getchunks)."""
    ino = os.stat(m.path / name).st_ino
    nodes = []
    for node in (2, 3, 1):
        rc, out = mgmt('raft-getchunks', c.ep(node), str(ino))
        found = re.findall(r'ci=\d+ nodes=(\d+),(\d+),(\d+)', out)
        if rc == 0 and found:
            for f in found:
                nodes.append(tuple(int(x) for x in f))
            return nodes
    raise AssertionError(f'no chunk records for {name} (ino {ino}): {out[-1500:]}')


def fragments(store):
    n = 0
    for dirpath, dirnames, files in os.walk(store):
        if '/mdraft' in dirpath or dirpath.endswith('/meta'):
            continue
        n += sum(1 for f in files if re.match(r'^\d+[._-]', f) or f.endswith('.frag'))
    return n


t0 = time.time()
c = None
mounts = []
writer = None
stop = threading.Event()
written, errors = {}, []
notes = []
try:
    c = Cluster([1, 2, 3, 4, 5])
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    end = time.time() + 60
    while time.time() < end and mgmt('raft-getattr', c.ep(1), '1')[0] != 0:
        time.sleep(0.3)

    # 1. Two mounts at revision 1.
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    b = Mount(c, 3, 'mnt-b')
    mounts.append(b)
    for m in (a, b):
        m.wait_log(r'membership view: revision 1, 3 members')
    pids = {m.name: m.p.pid for m in mounts}
    for k, size in enumerate((0, 1, 4096, MiB + 17, 3 * MiB + 5)):
        written[f'before-{k}'] = write(a, f'before-{k}', size)

    # 2. Continuous writes through mount A.
    def run_writer():
        k = 0
        while not stop.is_set():
            name = f'load-{k}'
            try:
                written[name] = write(a, name, MiB + 3 * k)
            except OSError as e:
                errors.append(f'{name}: {e}')
            k += 1
            time.sleep(0.05)
    writer = threading.Thread(target=run_writer, daemon=True)
    writer.start()
    time.sleep(1.0)

    # 3. Node 4 joins while the mounts serve and write.
    c.start(4, join=2)
    c.wait_log(4, r'raft-host: formed: 4 members')
    for m in (a, b):
        m.wait_log(r'membership view: revision 1 -> 2 from node \d+: added member 4 at '
                   rf'{HOST}:{c.ports[4]} \(4 members, no remount\)')
    after4 = []
    for k in range(8):
        written[f'after4-{k}'] = write(a, f'after4-{k}', 2 * MiB)
        after4.append(f'after4-{k}')
    on4 = [n for name in after4 for n in chunk_nodes(c, a, name)]
    assert any(4 in n for n in on4), f'no chunk of the new files on node 4: {on4}'
    assert all(max(n) <= 4 for n in on4), on4

    # 4. Node 5 joins.
    c.start(5, join=3)
    c.wait_log(5, r'raft-host: formed: 5 members')
    for m in (a, b):
        m.wait_log(r'membership view: revision 2 -> 3 from node \d+: added member 5 at '
                   rf'{HOST}:{c.ports[5]} \(5 members, no remount\)')
    after5 = []
    for k in range(8):
        written[f'after5-{k}'] = write(a, f'after5-{k}', 2 * MiB)
        after5.append(f'after5-{k}')
    on5 = [n for name in after5 for n in chunk_nodes(c, a, name)]
    assert any(5 in n for n in on5), f'no chunk of the new files on node 5: {on5}'
    time.sleep(1.0)
    stop.set()
    writer.join(timeout=120)
    assert not writer.is_alive(), 'writer stuck'
    assert not errors, errors[:10]
    frags = {i: fragments(c.dir / f'n{i}') for i in (4, 5)}
    assert frags[4] > 0 and frags[5] > 0, frags
    old = [n for k in (3, 4) for n in chunk_nodes(c, a, f'before-{k}')]
    assert all(max(n) <= 3 for n in old), f'old placement changed: {old}'

    # 5. Every file reads back exactly through both mounts; no remount.
    os.sync()
    for m in (b, a):
        for name, size in written.items():
            got = (m.path / name).read_bytes()
            assert got == payload(name, size), f'{m.name}: {name} differs ({len(got)} of {size})'
    loads = sum(1 for n in written if n.startswith('load-'))
    for m in mounts:
        assert m.p.poll() is None and m.p.pid == pids[m.name], f'{m.name} restarted'
        assert os.path.ismount(m.path), f'{m.name} not mounted'
        log = m.log()
        assert len(re.findall(r'Discovered \d+ cluster nodes', log)) == 1, log[-2000:]
        assert len(re.findall(r'membership view: revision \d+, \d+ members', log)) == 1, log[-2000:]
        assert 'membership view: refused' not in log, log[-2000:]
    notes.append(f'{len(written)} files exact through both mounts ({loads} written under load '
                 f'during both additions); fragments on node 4: {frags[4]}, node 5: {frags[5]}; '
                 f'chunks of post-install files on 4: {sum(4 in n for n in on4)}/{len(on4)}, '
                 f'on 5: {sum(5 in n for n in on5)}/{len(on5)}')
finally:
    stop.set()
    if writer:
        writer.join(timeout=60)
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
print('K33 client view: two mounts on 3 nodes install revisions 2 (node 4) and 3 (node 5) '
      'without a remount under continuous writes; new chunks placed on 4 and 5; old placement '
      f'kept; every file exact: PASS ({time.time() - t0:.1f} s)')
