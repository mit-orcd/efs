#!/usr/bin/env python3
"""W159: metadata operations on members with slow durable barriers.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and one efs-fuse mount; private stores under TMPDIR, free ports (never
17xxx). The three daemons run under tests/live/w159_slow_fsync.c, which adds
20 ms to every fsync/fdatasync and queues the three on one lock: the expiry
fixture's conditions (four members and the client on one host, a consumer
NVMe answering a durable write in 7-28 ms, queued), where the closing core
POSIX run timed out `dir_many_files` and `concurrent_creates_same_dir`.

A burst of 128 small-file creates from 16 threads, then their unlinks.
Checks:
 1. every create and unlink succeeds (no EBUSY, no EIO);
 2. no member answered BUSY from an expired wait for its own committed
    entry (`raft-obs: wait_timeouts_own=0` on every member); the expired
    ReadIndex waits of followers (`wait_timeouts`, kept at the fixed budget
    so the client asks the leader) and the client's slow RPCs after a
    BUSY/STALE retry (`inode-rpc: slow-ok ... saw_busy=1`, which has other
    sources such as an allocation intent) are recorded only;
 3. recorded only: how many grants the progress-bounded wait renewed
    (`wait_extends`), which depends on the host's timing.
Under these conditions the fixed 400 ms budget before apply_wait.h answered
BUSY 10 times on one member in one gate run (and up to 254 times per member
at 15 ms per barrier in the amd reproduction); the fix waits while the
apply advances. Heavier settings (50 ms per barrier, 256 creates) renew
hundreds of grants and still see a few waits outlive them, so they are not
a pass/fail condition here.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3, efs-fuse or cc.
"""
import os, re, shutil, subprocess, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, root  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok() or not shutil.which('cc'):
    print('W159 apply wait: NOTRUN (no /dev/fuse, fusermount3, efs-fuse or cc)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-w159-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env['EFS_RAFT_OBS'] = '1'
so = work / 'w159_slow_fsync.so'
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                str(root / 'tests/live/w159_slow_fsync.c'), '-ldl', '-o', str(so)], check=True)
slow = dict(LD_PRELOAD=str(so), EFS_TEST_FSYNC_DELAY_US='20000',
            EFS_TEST_FSYNC_LOCK_FILE=str(work / 'fsync.lock'))
THREADS, PER = 16, 8


def obs(c, i, key):
    """The last raft-obs value of `key` in member i's log (0 when none printed)."""
    vals = re.findall(rf'raft-obs: .*\b{key}=(\d+)', c.last_run(i))
    return int(vals[-1]) if vals else 0


c = Cluster(work, 'w', [1, 2, 3], env)
mount = None
try:
    c.start(1, extra_env=slow)
    c.start(2, join=1, extra_env=slow)
    c.start(3, join=1, extra_env=slow)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
    mount = Mount(c, 1, 'mnt')
    d = mount.path / 'burst'
    d.mkdir()
    errors = []

    def worker(t):
        for k in range(PER):
            try:
                (d / f'f{t}-{k}').write_bytes(b'w159')
            except OSError as e:
                errors.append(f'create f{t}-{k}: {e}')

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(t,)) for t in range(THREADS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    created = time.time() - t0
    names = sorted(os.listdir(d))
    assert not errors, errors[:5]
    assert len(names) == THREADS * PER, (len(names), THREADS * PER)
    for n in names:
        try:
            os.unlink(d / n)
        except OSError as e:
            errors.append(f'unlink {n}: {e}')
    assert not errors, errors[:5]
    assert os.listdir(d) == [], os.listdir(d)
    burst = time.time() - t0
    print(f'1. {THREADS * PER} creates in {created:.2f} s, creates and unlinks in {burst:.2f} s, '
          f'no error PASS')

    # The raft-obs line is printed at most every 5 s; wait one period so the
    # counters of the burst are on record.
    time.sleep(6)
    timeouts = {i: obs(c, i, 'wait_timeouts_own') for i in (1, 2, 3)}
    reads = {i: obs(c, i, 'wait_timeouts') for i in (1, 2, 3)}
    extends = {i: obs(c, i, 'wait_extends') for i in (1, 2, 3)}
    busy = re.findall(r'inode-rpc: slow-ok .*saw_busy=1.*', mount.logp.read_text(errors='replace'))
    assert all(v == 0 for v in timeouts.values()), (timeouts, reads, extends)
    # BUSY has other sources (an allocation intent, a STALE dentry, and a
    # follower's ReadIndex wait, which keeps its fixed budget so the client
    # asks the leader); the own-entry count decides, the rest is recorded.
    print(f'2. no expired own-entry wait: wait_timeouts_own {timeouts} PASS; all expired waits '
          f'{reads}, client slow RPCs after a BUSY/STALE retry: {len(busy)} (recorded)')
    # Whether some wait outlived one grant depends on the host's timing;
    # recorded, not asserted (at 50 ms per barrier every run renews
    # hundreds of grants, and a few waits then outlive even the renewals).
    print(f'3. progress-bounded wait renewals: wait_extends {extends} (recorded)')
    print('W159 apply wait: PASS')
finally:
    if mount:
        mount.close()
    c.stop_all()
    # Keep the ladder and server wait counters before discarding fixtures.
    # A failed workload stays available for investigation, never log-and-delete.
    failed = sys.exc_info()[0] is not None
    for log in c.dir.glob('*.log'):
        print(f'==== {log.name} ====\n'+log.read_text(errors='replace'), flush=True)
    if failed:
        print(f'W159 failed fixture retained: {work}', flush=True)
    else:
        subprocess.run([str(Path.home() / 'bin/edelete'), '--force', '--delete', str(work)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
