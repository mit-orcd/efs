#!/usr/bin/env python3
"""Task FE: room freed on a full member goes to a refused write's held chunk first.

K90 (tests/test_k90_unmount_enospc.py, mount B): a write refused with ENOSPC
leaves its chunk dirty in the mount, and the mount PUTs it again as soon as
room is back. So when an unlink frees one fragment on a full node, the mount
holding a refused chunk may take it ahead of a new write that fits. That is
the contract; it made `room_scenario` in test_b6_unavailable.py lose
`room-0` to the fill's last refused file on a gate host (`room: ENOSPC then
ENOSPC`). The B6 scenario now gives the hold up first (`give_up_held`).

Three private daemons, node 3 at 4 MiB, filled through the real refusal path
(no hook) with 128 KiB files (one 64 KiB fragment each on node 3). Mounts A
and B each leave one refused 128 KiB chunk held.
- A is unmounted while node 3 is full: exit 1, its chunk named, nothing held.
- One file is unlinked: the ledger drops by exactly one fragment. A fresh
  mount C writes one file: it lands in that room (the B6 scenario's case).
- Another file is unlinked. B's held chunk is flushed (fsync of the refused
  file): it lands in that room (the contract B6 had to respect).
Skipped (NOTRUN) without FUSE.
"""
import errno, os, re, tempfile, threading, time
from pathlib import Path
from k33_daemons import Cluster, Mount, fuse_ok, give_up_held, mgmt, payload

if not fuse_ok():
    print('B6 room claimant: NOTRUN (FUSE unavailable)')
    raise SystemExit(0)
QUOTA = 4 << 20
FRAGMENT = 65536
work = Path(tempfile.mkdtemp(prefix='efs-fe-room-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='600000')  # the packer must not free room under the test
c = Cluster(work, 'room', [1, 2, 3], env)


def used():
    rc, text = mgmt(env, 'gc-status', c.ep(3))
    m = re.search(r'\bdata_usage_bytes=(\d+)', text)
    assert rc == 0 and m, text
    return int(m[1])


def wait_used(want, what):
    """The ledger reaches `want` (polled; the bound only ends a hung run)."""
    end = time.monotonic() + 30
    while used() != want:
        assert time.monotonic() < end, f'{what}: ledger {used()}, want {want}'
        time.sleep(0.1)


def refused_close(m, name, out):
    """One 128 KiB write, no fsync: the refusal reaches close and the chunk stays held."""
    fd = os.open(m.path / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    assert os.write(fd, payload(name, 2 * FRAGMENT)) == 2 * FRAGMENT
    try:
        os.close(fd)
        out['errno'] = 0
    except OSError as e:
        out['errno'] = e.errno


try:
    c.start(1); c.start(2, join=1); c.start(3, join=1, quota=str(QUOTA >> 20) + 'M')
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a'); b = Mount(c, 1, 'mnt-b')
    # Fill: one more 128 KiB chunk no longer fits on node 3.
    now, fills = used(), 0
    while QUOTA - now >= FRAGMENT:
        fd = os.open(a.path / f'fill-{fills}', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        os.write(fd, payload(f'fill-{fills}', 2 * FRAGMENT)); os.fsync(fd); os.close(fd)
        fills += 1; now = used()
        assert fills < 80, 'quota never filled'
    ra, rb = {}, {}
    ts = [threading.Thread(target=refused_close, args=(a, 'held-a', ra)),
          threading.Thread(target=refused_close, args=(b, 'held-b', rb))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    assert ra['errno'] == errno.ENOSPC and rb['errno'] == errno.ENOSPC, (ra, rb)
    full = used()
    # A gives its chunk up: the unmount is the proof that it holds nothing.
    rc, named = give_up_held(a)
    assert (rc, named) == (1, 1), (rc, named)
    assert used() == full
    # One fragment of room: a new write from a mount without a hold gets it.
    os.unlink(b.path / 'fill-0')
    wait_used(full - FRAGMENT, 'first unlink')
    m = Mount(c, 1, 'mnt-c')
    fd = os.open(m.path / 'room-0', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
    try:
        assert os.write(fd, payload('room-0', 2 * FRAGMENT)) == 2 * FRAGMENT
        os.fsync(fd)
    finally:
        os.close(fd)
    wait_used(full, 'room-0')
    assert (m.path / 'room-0').read_bytes() == payload('room-0', 2 * FRAGMENT)
    # More room: B's held chunk lands in it as soon as it is flushed (K90).
    os.unlink(b.path / 'fill-1')
    wait_used(full - FRAGMENT, 'second unlink')
    fd = os.open(b.path / 'held-b', os.O_RDWR)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    wait_used(full, 'held chunk of B')
    print(f'B6 room claimant: A given up (exit 1, 1 chunk named), room-0 landed in the first '
          f"fragment, B's held chunk in the second ({fills} fills): PASS")
finally:
    c.stop_all()
