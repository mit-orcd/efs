#!/usr/bin/env python3
"""A small file rewritten across a pack-epoch boundary before it packed is
packed, not left as a candidate for minutes (D4, F1 follow-ups Open 5:
after a fixed-working-set rewrite load 28 candidates stayed unpacked with
a lag of 346 s, and the 75 s plateau gate missed its 150 s drain bound).

Background: the candidate index key is (group anchor, epoch, inode) with
epoch = publish time / EFS_PACK_EPOCH_S (60 s). A rewrite in a later epoch
adds a second key for the same inode with the same inode generation, and
both validate against the inode row. The selection sorted both into one
segment once the backlog fitted in one, efs_pack_build refused a segment
holding one inode twice, and the job gave up without a log line or a
counter: every pass built the same failing segment (`eligible=28
segments=1`, files_packed frozen) until the load happened to rewrite the
file again. Leases were not involved: the lag counts only lease-free,
quiet candidates.

Three efsd and one efs-fuse on loopback (free ports, stores under TMPDIR),
EFS_PACK_MIN_AGE_MS=MIN_AGE_S. FILES small files are written, then
rewritten in place every ROUND_S (inside the minimum age, so they never
become quiet) until a round has run wholly in a later epoch than an
earlier round: each file then has two candidate entries when it becomes
quiet. The test requires the pack backlog of every led group to reach 0
within BOUND_S of the files becoming quiet, files_packed to count every
file once, no packer put or read errors, and every file to read back with
its last content. On the unfixed server the backlog stays at 2 x FILES
(4 of 4 runs fail, one with this version of the test).
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, SLOW, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('pack duplicate candidate: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
EPOCH_S = 60                 # EFS_PACK_EPOCH_S
FILES = 24
MIN_AGE_S = 8
ROUND_S = 2
BOUND_S = 20.0 * SLOW
work = Path(tempfile.mkdtemp(prefix='efs-pdc-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS=str(MIN_AGE_S * 1000), EFS_PACK_INTERVAL_MS='200')
t0 = time.time()


def pack(c):
    rc, out = mgmt(c.env, 'pack-compact', c.ep(1), '0')
    assert rc == 0, out
    backlog = {int(g): int(n) for g, n in re.findall(r'backlog group=(\d+) candidates=(\d+)', out)}
    tot = {k: sum(int(x) for x in re.findall(rf'\b{k}=(\d+)', out))
           for k in ('files_packed', 'put_errors', 'read_errors')}
    return backlog, tot


def wait_epoch_second(lo, hi):
    """Sleep until the wall clock's second within the epoch is in [lo, hi)."""
    while not lo <= time.time() % EPOCH_S < hi:
        time.sleep(0.2)


c = Cluster(work, 'c', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    m = Mount(c, 1, 'mnt')
    mounts.append(m)
    d = m.path / 'd'
    d.mkdir()
    names = [d / f'f{i}' for i in range(FILES)]
    first_epoch, v = None, 0
    while True:
        v += 1
        start = time.time()
        for p in names:             # in place after the first: an O_TRUNC would show the packer size 0
            with open(p, 'r+b' if v > 1 else 'wb') as f:
                f.write(payload(f'{p.name}.v{v}', 2048))
        end = time.time()
        assert end - start < MIN_AGE_S - ROUND_S, f'a round of writes took {end - start:.1f} s'
        epoch = int(start // EPOCH_S)
        if epoch == int(end // EPOCH_S):        # the round lies in one epoch
            if first_epoch is None:
                first_epoch = epoch
            elif epoch != first_epoch:
                break
        time.sleep(max(0.0, start + ROUND_S - time.time()))
    quiet = time.time() + MIN_AGE_S
    time.sleep(2)                   # a packer pass recounts the index
    backlog, before = pack(c)
    assert sum(backlog.values()) >= 2 * FILES, f'expected two candidate entries per file: {backlog}'
    while time.time() < quiet:
        time.sleep(0.5)
    end = quiet + BOUND_S
    while time.time() < end:
        backlog, tot = pack(c)
        if backlog and not any(backlog.values()):
            break
        time.sleep(0.5)
    waited = round(time.time() - quiet, 1)
    assert backlog and not any(backlog.values()), \
        f'pack backlog {backlog} {BOUND_S} s after the files became quiet (files_packed ' \
        f'{tot["files_packed"] - before["files_packed"]} of {FILES})'
    packed = tot['files_packed'] - before['files_packed']
    assert packed == FILES, f'files_packed {packed}, want {FILES}'
    assert not tot['put_errors'] and not tot['read_errors'], f'packer errors: {tot}'
    for p in names:
        assert p.read_bytes() == payload(f'{p.name}.v{v}', 2048), f'{p} differs'
    print(f'{FILES} files with two candidate entries each ({v} rounds): backlog 0 {waited} s after quiet '
          f'(bound {BOUND_S}), {packed} packed, 0 put/read errors, contents exact PASS')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_PDC_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'pack duplicate candidate: PASS ({time.time() - t0:.1f} s)')
