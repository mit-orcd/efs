#!/usr/bin/env python3
"""K110 (D42 F23 as amended): the final fsync after sequential SIGKILLs.

Three nodes, one mount. A file is written without fsync and every node's
durability barrier covers it. Node 3 is SIGKILLed and restarted, then node 2
is: each came back with a new PUT verifier, so before K110 the client counted
both nodes' fragments of the file lost (`LLD`) and the fsync failed with EIO.
Each restarted node now reports its predecessor's last completed barrier in
its COMMIT reply, and the client counts the fragments that barrier covered
durable. The fsync must succeed with no degraded mark, and the client must say
how many fragments the barrier history proved.

A second file is written while node 3 is down (Task EY's DML stripes: one
member's fragment missing, one covered by a barrier, one durable) and node 2
is killed after the barrier: its fsync, and a later one, succeed too.
A fragment acknowledged after the barrier stays lost; that rule is checked on
the units (tests/test_data_commit.c, tests/test_store_durability.c) and with a
daemon crash in tests/test_k36_recovery.py step 4.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, signal, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K110 barrier history: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k110-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
SIZE = 3 * 1024 * 1024 + 5
t0 = time.time()


def synced(c, i):
    """(fragments still unsynced, MiB acknowledged) summed over node i's roots."""
    rc, out = mgmt(c.env, 'sync-status', c.ep(i))
    unsynced = sum(int(n) for n in re.findall(r'unsynced=[\d.]+ MiB \((\d+) fragments', out))
    acked = sum(float(n) for n in re.findall(r'of ([\d.]+) MiB acked', out))
    return unsynced, acked


def wait_barrier(c, ids, timeout=60):
    """Every node acknowledged the file and a barrier made it durable."""
    end, last = time.time() + timeout, None
    while time.time() < end:
        last = {i: synced(c, i) for i in ids}
        if all(u == 0 and a >= 1.0 for u, a in last.values()):
            return
        time.sleep(0.3)
    raise AssertionError(f'no barrier covered the file: {last}')


def verified(c, i, timeout=120):
    c.wait_log(i, r'recovery: verified in \d+ ms', timeout=timeout)


def up(c, i, timeout=90):
    c.wait_log(i, rf'raft-host: up raft_id={i - 1} n=3 formed', timeout=timeout)


c = Cluster(work, 'k110', [1, 2, 3], env)
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a')
    data = payload('W', SIZE)
    with open(a.path / 'W', 'wb') as f:
        f.write(data)
    wait_barrier(c, (1, 2, 3))
    for i in (3, 2):
        c.stop(i, sig=signal.SIGKILL)
        c.restart(i)
        up(c, i)
        verified(c, i)
    old = a.logp.read_text().count('\n')
    with open(a.path / 'W', 'rb+') as f:
        os.fsync(f.fileno())   # OSError (EIO) before K110: two nodes' fragments were lost
    tail = ''.join(a.logp.read_text().splitlines(True)[old:])
    m = re.search(r'fsync: restarted members: (\d+) fragments proven by their barrier history, '
                  r'(\d+) lost', tail)
    assert m, ('the client did not count the barrier history', tail[-2000:])
    proven, lost = int(m.group(1)), int(m.group(2))
    assert proven >= 2 and lost == 0, (proven, lost, tail[-2000:])
    assert 'marked degraded' not in tail, ('a covered fragment was marked, not proven', tail[-2000:])
    assert (a.path / 'W').read_bytes() == data
    # Task EY's pattern (tcp-h01-k110): a stripe written while node 3 was down (its
    # fragment M: never acknowledged, the publication carries the degraded
    # mark), then node 2, whose fragment the barrier covered, is SIGKILLed and
    # restarted. Node 1 holds D, node 2's L is proven by its history, M is
    # covered by the mark: the fsync must succeed, and so must a later one.
    c.stop(3, sig=signal.SIGKILL)
    data2 = payload('X', SIZE)
    with open(a.path / 'X', 'wb') as f:
        f.write(data2)
    c.restart(3)
    up(c, 3)
    verified(c, 3)
    wait_barrier(c, (1, 2))
    c.stop(2, sig=signal.SIGKILL)
    c.restart(2)
    up(c, 2)
    verified(c, 2)
    old = a.logp.read_text().count('\n')
    for _ in range(2):   # the later fsync of the same stripes too, every member back
        with open(a.path / 'X', 'rb+') as f:
            os.fsync(f.fileno())
    tail = ''.join(a.logp.read_text().splitlines(True)[old:])
    m2 = re.search(r'fsync: restarted members: (\d+) fragments proven by their barrier history, '
                   r'(\d+) lost', tail)
    assert m2 and int(m2.group(1)) >= 1 and int(m2.group(2)) == 0, (m2, tail[-2000:])
    assert 'need 2' not in tail, tail[-2000:]
    assert (a.path / 'X').read_bytes() == data2
    print(f'K110 barrier history: fsync after SIGKILL of node 3 then node 2 proves {proven} '
          f'fragments from the restarted members\' barriers (none lost, no degraded mark) '
          f'PASS ({time.time() - t0:.1f} s)')
finally:
    c.stop_all()
