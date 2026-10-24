#!/usr/bin/env python3
"""K33 acceptance: exact payloads, packed and unpacked, across additions.

Real efsd daemons on loopback (tests/k33_daemons.py) and efs-fuse; private
stores under TMPDIR, free ports (never 17xxx), discovery off, GC on, the
packer on with a 500 ms minimum age. Small files (1 B to 16 KiB, at most
EFS_PACK_THRESHOLD) are written in batches and the test waits until the
packer has moved each batch into segments (its `packed_total` counters
grow by the batch); large files (64 KiB + 1 to 1 MiB + 3) are never packed.
1. Three nodes form; batch S0 and large files L0 through mount A (node 1).
2. A writer thread keeps writing large files through mount A for the whole
   run (open/write/close, no fsync: DATA_COMMIT needs every storage node,
   W154 limitation); each written file is read back at once; a failed
   write is retried until it succeeds.
3. Node 4 joins (through node 2), batch S1 and L1; node 5 joins (through
   node 3), batch S2 and L2. Mount B (through node 4) reads everything.
4. Orderly restart of the whole cluster: writer paused, mounts closed,
   every daemon stopped with SIGTERM and started again at once from its
   store; formed n=5 again with the same ids; mount A again, writer
   resumes; everything reads back.
5. Each single-member outage at five members, node 1 to node 5 in turn:
   SIGTERM the node, write batch S3.k and large files while it is down (the
   writer keeps going), read every file written so far through mount A,
   restart the node, wait until it is up, then read everything again.
6. A fresh mount through node 5 reads every file: every byte matches.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, payload, voters_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K33 payload: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k33p-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_MEMBER_EXIT')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
env.pop('EFS_GC_DISABLE', None)
SMALL = (1, 17, 512, 4095, 4096, 9000, 16384)
LARGE = (64 * 1024 + 1, 300000, 1024 * 1024 + 3)
files = {}          # name -> size, every file whose write returned success
lock = threading.Lock()
notes = []


def packed_total(c):
    """Files the packers switched into segments, summed over every daemon
    run and metadata group (counters restart with a process or a term)."""
    total = 0
    for i in c.ports:
        if not (c.dir / f'n{i}.log').exists():
            continue
        for run in c.log(i).split('==== start')[1:]:
            last = {}
            for g, t, n in re.findall(r'packer: group=(\d+) term=(\d+) .*?packed_total=(\d+)', run):
                last[(g, t)] = int(n)
            total += sum(last.values())
    return total


def write_batch(m, tag, sizes):
    for k, size in enumerate(sizes):
        name = f'{tag}-{k}'
        put(m, name, size)


def put(m, name, size, deadline=60):
    end = time.time() + deadline
    while True:
        try:
            (m.path / name).write_bytes(payload(name, size))
            break
        except OSError as e:
            if time.time() > end:
                raise AssertionError(f'{name}: write never succeeded: {e}')
            with lock:
                stats['retries'] += 1
            time.sleep(0.2)
    with lock:
        files[name] = size


def wait_packed(c, base, n, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        if packed_total(c) >= base + n:
            return packed_total(c)
        time.sleep(0.2)
    raise AssertionError(f'packer moved {packed_total(c) - base} of {n} files')


def verify(m, what):
    with lock:
        snap = dict(files)
    for name, size in snap.items():
        got = (m.path / name).read_bytes()
        assert got == payload(name, size), f'{what}: {name} differs ({len(got)} of {size} B)'
    return len(snap)


stats = {'retries': 0, 'written': 0}
run_writer = threading.Event()
stop_writer = threading.Event()
writer_err = []


def writer(mref):
    k = 0
    while not stop_writer.is_set():
        if not run_writer.wait(0.1):
            continue
        name, size = f'w-{k}', LARGE[k % len(LARGE)]
        step = 'write'
        try:
            put(mref[0], name, size)
            step = 'read back'
            assert (mref[0].path / name).read_bytes() == payload(name, size), name
        except Exception as e:  # noqa: BLE001 - reported by the main thread
            stamp = time.strftime('%H:%M:%S', time.gmtime()) + f'.{int(time.time() * 1000) % 1000:03d}Z'
            writer_err.append(f'{name} {step} at {stamp}: {e!r}')
            return
        stats['written'] += 1
        k += 1


t0 = time.time()
c = Cluster(work, 'p', [1, 2, 3, 4, 5], env)
mounts = []
th = None
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    mref = [a]
    base = packed_total(c)
    write_batch(a, 's0', SMALL)
    write_batch(a, 'l0', LARGE)
    base = wait_packed(c, base, len(SMALL))
    th = threading.Thread(target=writer, args=(mref,), daemon=True)
    th.start()
    run_writer.set()

    # 3. Additions under the writer.
    for node, via, tag in ((4, 2, '1'), (5, 3, '2')):
        c.start(node, join=via)
        c.wait_log(node, rf'raft-host: up raft_id={node - 1} n={node} formed', timeout=60)
        c.wait_members(list(range(1, node + 1)), node)
        write_batch(a, f's{tag}', SMALL)
        write_batch(a, f'l{tag}', LARGE)
        base = wait_packed(c, base, len(SMALL))
    b = Mount(c, 4, 'mnt-b')
    mounts.append(b)
    n = verify(b, 'after additions (mount B)')
    b.close()
    mounts.remove(b)
    assert not writer_err, writer_err

    # 4. Orderly restart of the whole cluster.
    run_writer.clear()
    time.sleep(0.5)
    a.close()
    mounts.remove(a)
    for i in (5, 4, 3, 2, 1):
        c.stop(i)
    c.start(1)
    for i in (2, 3, 4, 5):
        c.start(i, join=1)
    for i in (1, 2, 3, 4, 5):
        c.wait_log(i, rf'raft-host: up raft_id={i - 1} n=5 formed', timeout=60)
        assert c.node_id(i) == i, i
    c.wait_members([1, 2, 3, 4, 5], 5)
    c.wait_voters([1, 2, 3, 4, 5], lambda i, v: voters_ok(i, v, 5))
    a = Mount(c, 1, 'mnt-a2')
    mounts.append(a)
    mref[0] = a
    verify(a, 'after the orderly restart')
    run_writer.set()

    # 5. Each single-member outage at five members.
    for k in (1, 2, 3, 4, 5):
        base = packed_total(c)
        c.stop(k)
        write_batch(a, f's3.{k}', SMALL)
        write_batch(a, f'l3.{k}', LARGE)
        verify(a, f'node {k} down')
        c.restart(k)
        c.wait_log(k, rf'raft-host: up raft_id={k - 1} n=5 formed', timeout=60)
        c.wait_members([1, 2, 3, 4, 5], 5)
        c.wait_voters([1, 2, 3, 4, 5], lambda i, v: voters_ok(i, v, 5))
        wait_packed(c, base, len(SMALL))
        verify(a, f'node {k} back')
        assert not writer_err, writer_err
    stop_writer.set()
    th.join(timeout=60)
    assert not writer_err, writer_err

    # 6. A fresh mount through a storage-only member reads everything.
    d = Mount(c, 5, 'mnt-d')
    mounts.append(d)
    n = verify(d, 'final (mount D)')
    nsmall = sum(1 for s in files.values() if s <= 16384)
    notes.append(f'{n} files exact ({nsmall} small, packed; {n - nsmall} large); writer '
                 f'{stats["written"]} files, {stats["retries"]} retried writes; '
                 f'packed_total {packed_total(c)}')
finally:
    stop_writer.set()
    run_writer.set()
    if th:
        th.join(timeout=30)
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K33_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
for n in notes:
    print(n)
print('K33 payloads: packed and unpacked files exact across additions 3->4->5 under a writer, '
      f'an orderly restart of all five and each single-member outage: PASS ({time.time() - t0:.1f} s)')
