#!/usr/bin/env python3
"""K38/W161: writers share chunks of one file under dcache pressure, over FUSE.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build,
private stores under TMPDIR, free ports, never 17xxx) and two efs-fuse
processes, each through its own member. 24 writer threads on the mounts write
one shared file in the ior hard layout: record k of writer r at offset
(k * writers + r) * 47008, so most 128 KiB chunks hold spans of two or three
writers, which race on the chunk's dcache slot, its PUTs and its REPORTs
(the orcd-mini ior hard shape, 8 ranks on one client, Oct 25). The client's
body budget is the 32 MiB minimum (EFS_DCACHE_HARD_BYTES), its reclaim
target 1 MiB (EFS_DCACHE_BYTES), and 24 writes in flight reserve more than
the budget, so admission waits on the drain (`dcache-pressure` lines) while
spans of shared chunks are PUT, reported and replayed against each other.
Both mounts read the shared chunks back.

Checks: no write, fsync or close fails (the EIO of K38); every byte reads
back exactly through both mounts and after a remount (the zero-filled record
of the install-order finding); the writing client logged no record the
REPORT bound stopped (`report-stuck`) and no lost span (`report-span-lost`);
it ran under pressure. Prints the STALE replay and superseded-span counts.

By default the writers are split over both mounts (`--clients 2`), so
the two clients' REPORT units contend for the same chunks across groups.
Before K40 that failed about half the runs on devel 5af760ff1 with EIO:
a forwarded PREPARE batch refused for one CAS-lost chunk left the other
chunks' stages undecided (no ABORT) until txn-recover, and every REPORT
touching them answered BUSY for the whole 8 s budget
(docs/status/k40-two-client-unit-busy-20261025.md). `--clients 1` keeps
the single-client K38 shape.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K38 shared chunks under pressure: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)

import argparse  # noqa: E402
ap = argparse.ArgumentParser(description='K38 shared chunks under dcache pressure')
ap.add_argument('--writers', type=int, default=24)
ap.add_argument('--clients', type=int, choices=(1, 2), default=2,
                help='writing clients (1: the single-client K38 shape)')
ap.add_argument('--records', type=int, default=200, help='records per writer')
ap.add_argument('--fsyncs', type=int, default=1, help='fsyncs per writer, the last at the end')
ap.add_argument('--no-pressure-check', action='store_true')
opt = ap.parse_args()
RANKS, XFER, CHUNK = opt.writers, 47008, 128 << 10
SEGMENTS = opt.records
FSYNC_EVERY = max(1, SEGMENTS // opt.fsyncs)
work = Path(tempfile.mkdtemp(prefix='efs-k38-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
client_env = dict(env, EFS_DCACHE_HARD_BYTES=str(32 << 20), EFS_DCACHE_BYTES=str(1 << 20))
t0 = time.time()


def record(r, k):
    return payload(f'k38-{r}-{k}', XFER)


def writer(m, r, errors):
    try:
        fd = os.open(m.path / 'shared', os.O_WRONLY)
        try:
            for k in range(SEGMENTS):
                off = (k * RANKS + r) * XFER
                assert os.pwrite(fd, record(r, k), off) == XFER
                if (k + 1) % FSYNC_EVERY == 0:
                    os.fsync(fd)
            os.fsync(fd)
        finally:
            os.close(fd)
    except Exception as e:  # noqa: BLE001 - reported with its writer
        errors.append(f'writer {r}: {e!r}')


def verify(m, what):
    with open(m.path / 'shared', 'rb') as f:
        data = f.read()
    assert len(data) == RANKS * SEGMENTS * XFER, f'{what}: size {len(data)}'
    bad = [(r, k) for k in range(SEGMENTS) for r in range(RANKS)
           if data[(k * RANKS + r) * XFER:(k * RANKS + r + 1) * XFER] != record(r, k)]
    lines = []
    for r, k in bad[:4]:
        o = (k * RANKS + r) * XFER
        got, want = data[o:o + XFER], record(r, k)
        diff = [i for i in range(XFER) if got[i] != want[i]]
        lines.append(f'writer {r} record {k}: bytes {o + diff[0]}..{o + diff[-1]} '
                     f'({len(diff)} differ, {got[diff[0]:diff[-1] + 1].count(0)} zero), '
                     f'chunks {(o + diff[0]) // CHUNK}..{(o + diff[-1]) // CHUNK}')
    assert not bad, f'{what}: {len(bad)} records differ\n' + '\n'.join(lines)


c = Cluster(work, 'k', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a', env=client_env)
    mounts.append(a)
    b = Mount(c, 2, 'mnt-b', env=client_env)
    mounts.append(b)
    with open(a.path / 'shared', 'wb') as f:
        f.truncate(RANKS * SEGMENTS * XFER)
    errors = []
    threads = [threading.Thread(target=writer, args=((a, b)[r % opt.clients], r, errors))
               for r in range(RANKS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=600)
    assert not any(t.is_alive() for t in threads), 'a writer did not finish in 600 s'
    logs = {m.path.name: m.logp.read_text(errors='replace') for m in mounts}
    assert not errors, '\n'.join(errors) + '\n' + '\n'.join(
        f'-- {n}:\n' + '\n'.join(l[-3000:].splitlines()[-30:]) for n, l in logs.items())
    write_s = time.time() - t0
    # Both views first, so a failure says whether the writer's cache or
    # the published data is wrong.
    seen = []
    for m in (a, b):
        try:
            verify(m, m.path.name)
        except AssertionError as e:
            seen.append(str(e))
    assert not seen, '\n'.join(seen)
    for n, l in logs.items():
        for bad in ('report-stuck ', 'report-span-lost '):
            assert bad not in l, f'{n}: {bad.strip()}\n' + '\n'.join(
                x for x in l.splitlines() if bad in x)[:3000]
    pressure = sum(l.count('dcache-pressure ') for l in logs.values())
    assert pressure or opt.no_pressure_check, 'no client ran under dcache pressure:\n' + '\n'.join(
        l[-2000:] for l in logs.values())
    stale = sum(len(re.findall(r'report-stale: chunks=', l)) for l in logs.values())
    superseded = sum(l.count(' superseded by seq=') for l in logs.values())
    for m in mounts:
        m.close()
    mounts = []
    r = Mount(c, 3, 'mnt-r', env=client_env)
    mounts.append(r)
    verify(r, 'remount')
    print(f'pressure lines {pressure}, STALE replay rounds {stale}, superseded spans {superseded}, '
          f'write+fsync {write_s:.1f} s')
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K38_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'K38 shared chunks under dcache pressure: {RANKS} writers on {opt.clients} client(s), no EIO, '
      f'{RANKS * SEGMENTS} records exact on both mounts and after remount PASS '
      f'({time.time() - t0:.1f} s)')
