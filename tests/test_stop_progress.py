#!/usr/bin/env python3
"""A11 finding 1: a clean client stop follows the client's stop progress.

Three efsd daemons on loopback (tests/k33_daemons.py, tests/faults/efsd)
with EFS_FAULT_REVOKE_BEGIN_FILE, and efs-fuse:
1. Busy client: mount A creates DIRS directories with one file each; a
   new directory's inode lands on its own metadata shard (files share
   their directory's), so the session touches thousands of shards.
   `scripts/client.sh stop` succeeds; the
   revoke is logged with its touched count and duration.
2. Small client: mount B writes one file; the stop succeeds within
   SMALL_STOP_S.
3. Stalled, then failed revoke: mount C drains and unmounts as client.sh
   does, with the fault file present, so every session BEGIN is held. The
   retire step (`client_processes.py retire --stall 3`) reports that the
   stop made no progress in phase revoking, and the client stays alive (no
   forced kill). The file then reads "fail": BEGIN is refused, the client
   exits 1, and retire reports the failed revoke with its reason.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, payload  # noqa: E402

DIRS = 3000
SMALL_STOP_S = 5
root = Path(__file__).resolve().parents[1]
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('stop progress: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-stop-progress-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
hold = work / 'revoke-hold'
env['EFS_FAULT_REVOKE_BEGIN_FILE'] = str(hold)


def client(*args, timeout=900):
    t = time.monotonic()
    p = subprocess.run(list(args), cwd=root, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr, time.monotonic() - t


def revoked(m):
    log = m.logp.read_text(errors='replace')
    found = re.findall(r'session revoked at unmount: epoch=\d+ touched=(\d+) rc=(-?\d+) ms=(\d+) '
                       r'fence_ms=(\d+) cleanup_ms=(\d+)', log)
    assert found, log[-3000:]
    return tuple(int(x) for x in found[-1])


t0 = time.time()
c = Cluster(work, 'p', [1, 2, 3], env)
mounts = []
notes = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')

    # 1. Busy client.
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    t = time.monotonic()
    for k in range(DIRS):
        (a.path / f'd{k}').mkdir()
        (a.path / f'd{k}' / 'f').write_bytes(payload(f'stop-{k}', 64))
    made = time.monotonic() - t
    rc, out, took = client('bash', 'scripts/client.sh', 'stop', str(a.path))
    assert rc == 0 and f'Unmounted {a.path}' in out, out + a.logp.read_text(errors='replace')[-3000:]
    assert a.p.wait(timeout=30) == 0
    mounts.remove(a)
    touched, vrc, ms, fence_ms, cleanup_ms = revoked(a)
    assert vrc == 0 and touched >= 1000, (touched, vrc)
    notes.append(f'busy: {DIRS} directories in {made:.1f} s, touched={touched}, revoke {ms} ms '
                 f'(fence {fence_ms}, cleanup {cleanup_ms}), stop {took:.1f} s')

    # 2. Small client.
    b = Mount(c, 2, 'mnt-b')
    mounts.append(b)
    (b.path / 'one').write_bytes(payload('one', 4096))
    rc, out, took = client('bash', 'scripts/client.sh', 'stop', str(b.path))
    assert rc == 0 and f'Unmounted {b.path}' in out, out
    assert b.p.wait(timeout=30) == 0
    mounts.remove(b)
    touched, vrc, ms, _, _ = revoked(b)
    assert vrc == 0 and took < SMALL_STOP_S, (took, ms)
    notes.append(f'small: touched={touched}, revoke {ms} ms, stop {took:.1f} s')

    # 3. Stalled, then failed revoke; the steps of client.sh stop by hand so
    # the retire step can take a short stall window.
    m = Mount(c, 3, 'mnt-c')
    mounts.append(m)
    (m.path / 'one').write_bytes(payload('held', 4096))
    rc, owner, _ = client(sys.executable, 'scripts/client_processes.py', 'has', str(m.path))
    assert rc == 0 and owner.split()[0] == str(m.p.pid), owner
    rc, out, _ = client('./efs-fuse', '--stop', str(m.path))
    assert rc == 0, out
    hold.touch()
    rc, out, _ = client('fusermount3', '-u', str(m.path))
    assert rc == 0, out
    rc, out, took = client(sys.executable, 'scripts/client_processes.py', 'retire', str(m.path),
                           *owner.split(), '--stall', '3')
    assert rc == 2 and 'no progress for 3 s in phase revoking' in out, out
    assert 'no forced kill' in out and m.p.poll() is None, out
    c.wait_log(1, r'EFS_FAULT_REVOKE_BEGIN_FILE present', run=False)
    notes.append(f'stalled: retire refused after {took:.1f} s, client alive')
    hold.write_text('fail\n')
    rc, out, took = client(sys.executable, 'scripts/client_processes.py', 'retire', str(m.path),
                           *owner.split(), '--stall', '30')
    assert rc == 2 and 'failed: revoke rc=-1 phase=begin' in out, out + m.logp.read_text(errors='replace')[-2000:]
    assert m.p.wait(timeout=30) == 1
    mounts.remove(m)
    notes.append(f'failed: {out.strip().splitlines()[-1]}')
finally:
    if hold.exists():
        hold.unlink()
    for x in mounts:
        x.close()
    c.stop_all()
    shutil.rmtree(work, ignore_errors=True)
print('\n'.join(notes))
print('stop progress: busy client stop succeeds, small stop fast, stalled and failed revoke '
      f'reported without a forced kill: PASS ({time.time() - t0:.1f} s)')
