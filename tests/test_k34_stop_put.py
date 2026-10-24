#!/usr/bin/env python3
"""K34: a member that stops in order while a multi-chunk write is in flight.

Three real efsd daemons on loopback (tests/k33_daemons.py) and efs-fuse;
every stripe uses all three members. The interleaving of the K34 failure
(tests/test_k33_payload.py, 4 of 6 runs on nuc) is forced with two test
hooks (src/server/put_upload.c):
- node 2 gets EFS_TEST_PUT_STOP_AFTER=2: right after it stores its second
  fragment it raises SIGTERM on itself, so its orderly stop begins with
  that PUT in hand;
- node 3 gets EFS_TEST_PUT_BUSY_FILE: while the file exists it answers
  every fragment PUT BUSY, as its ticket read did while node 2, the group
  leader, went away.
One 8 MiB file is written through mount A (node 1) while node 3 is BUSY;
the test removes the BUSY file once node 2 has exited (status 0).
Without the fix the first chunks were acknowledged on nodes 1 and 2 with
node 3 missing, and read EIO with node 2 down. Now:
1. Node 2 never acknowledges the fragment it stored while stopping (its
   log names the refusal), and the client resends node 3's fragment until
   it is stored instead of acknowledging the stripe degraded.
2. With node 2 down, the file reads back exactly through mount A and
   through a fresh mount on node 3; every degraded acknowledgement names
   node 2, the member that went away, never node 3.
3. Node 2 restarts; the file still reads back exactly.
4. Stale down-mark (B6): mount A marked node 2 down in step 1 (a 30 s
   cooldown) and it is back. A small file packed at the start into a
   segment on all three members (written through its own mount) must read
   back through mount A with node 3 stopped, from nodes 1 and 2: the read
   probes node 2 instead of skipping it.
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
    print('K34 stop during PUT: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k34-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_TEST_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
env.pop('EFS_GC_DISABLE', None)
busy = work / 'node3-busy'
SIZE = 8 * 1024 * 1024 + 5


def packed_total(c):
    """Files the packers switched into segments, summed over every daemon
    run and metadata group (as in tests/test_k33_payload.py)."""
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


t0 = time.time()
c = Cluster(work, 'p', [1, 2, 3], env)
mounts = []
result = {}
try:
    c.start(1)
    c.start(2, join=1, extra_env={'EFS_TEST_PUT_STOP_AFTER': '2'})
    c.start(3, join=1, extra_env={'EFS_TEST_PUT_BUSY_FILE': str(busy)})
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    # 0. A small file, packed into a segment on all three members, written
    # through its own mount so mount A never caches it.
    base = packed_total(c)
    w = Mount(c, 1, 'mnt-w')
    small = payload('k34-small', 4096)
    (w.path / 'small').write_bytes(small)
    w.close()
    end = time.time() + 60
    while packed_total(c) <= base and time.time() < end:
        time.sleep(0.2)
    assert packed_total(c) > base, 'the small file was never packed'
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    busy.touch()

    def write():
        try:
            (a.path / 'f').write_bytes(payload('k34', SIZE))
            result['rc'] = 0
        except OSError as e:
            result['rc'] = e

    th = threading.Thread(target=write, daemon=True)
    th.start()
    rc = c.wait_exit(2, timeout=60)
    stopped = time.time()  # mount A marks node 2 down about now, for 30 s
    assert rc == 0, f'node 2 stop was not orderly: {rc}\n{c.log(2)[-3000:]}'
    c.wait_log(3, r'EFS_TEST_PUT_BUSY_FILE present', run=False)
    busy.unlink()
    th.join(timeout=90)
    assert not th.is_alive(), 'write still running 90 s after node 2 stopped'
    assert result.get('rc') == 0, f'write failed: {result.get("rc")}\n' + \
        a.logp.read_text(errors='replace')[-3000:]
    # 2. Node 2 down: exact through mount A and a fresh mount on node 3.
    want = payload('k34', SIZE)
    assert (a.path / 'f').read_bytes() == want, 'mount A: f differs with node 2 down'
    d = Mount(c, 3, 'mnt-d')
    mounts.append(d)
    assert (d.path / 'f').read_bytes() == want, 'mount D: f differs with node 2 down'
    n2 = c.log(2)
    assert 'EFS_TEST_PUT_STOP_AFTER=2 reached' in n2, n2[-3000:]
    assert re.search(r'put-upload: stopping; fragment not acknowledged', n2), n2[-3000:]
    alog = a.logp.read_text(errors='replace')
    degraded = re.findall(r'degraded acknowledgement, fragment \d+ for node (\d+) not', alog)
    assert all(n == '2' for n in degraded), f'a stripe acknowledged without node {degraded}\n' + \
        alog[-3000:]

    # 3. Node 2 returns.
    c.restart(2)
    c.wait_log(2, r'raft-host: up raft_id=1 n=3 formed', timeout=60)
    c.wait_members([1, 2, 3], 3)
    assert (d.path / 'f').read_bytes() == want, 'mount D: f differs after node 2 returned'

    # 4. The packed file, node 3 stopped, while mount A still marks node 2 down.
    c.stop(3)
    late = time.time() - stopped
    assert late < 28, f'step 4 began {late:.1f} s after node 2 stopped: the client cooldown may be over'
    assert (a.path / 'small').read_bytes() == small, 'mount A: packed file differs with node 3 down'
    note = (f'{len(degraded)} degraded acknowledgements (all node 2), '
            f'{alog.count("attempt")} retried PUT attempts logged, packed read {late:.1f} s '
            'after node 2 stopped')
finally:
    if busy.exists():
        busy.unlink()
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K34_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(note)
print('K34 orderly stop with a multi-chunk write in flight: no acknowledgement from the stopping '
      'member, BUSY member completed, exact with it down and after it returned, packed read past a stale down-mark: '
      f'PASS ({time.time() - t0:.1f} s)')
