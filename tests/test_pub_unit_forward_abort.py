#!/usr/bin/env python3
"""A W77 publication unit whose forwarded PREPARE batch is refused after it
applied is aborted at once, not left to the recovery scan (F1 follow-up,
found by tests/test_d4_plateau.py over 30 minutes: three rewrites EIO).

Background: a multi-chunk write publishes as one cross-group transaction.
The node running the REPORT proposes the PREPAREs of the group it leads
locally (their verdicts come back through counted refs) and forwards the
other group's TXN_BATCH to that group's leader, whose answer is the
batch's verdict. A TXN_BATCH applies each PREPARE on its own, so a refused
batch can still have installed intents. When the forwarded batch was the
first one submitted, the refusal left no ref, the unit skipped its ABORT,
and the intents stayed undecided until the 5 s recovery scan
(`txn-recover ... -> ABORT`); the client's next publication of the inode
(a new id after STALE) was BUSY for that long and its 8 s budget ran out.

Three efsd of the EFS_FAULTS build and one efs-fuse on loopback (free
ports, stores under TMPDIR). EFS_FAULT_FWD_TXN_STALE=<dir> makes a leader
answer the next forwarded TXN_BATCH STALE after it applied, once per flag
file <dir>/stale-<node id>. For each of ROUNDS fresh 1 MiB files (8
chunks: a unit spanning both groups) every member's flag is armed, the file
is written and fsynced, and the test checks: the fault fired, write+fsync
took under BOUND_S (the recovery scan alone takes 5 s), the file reads back
exact, and no member logs a txn-recover ABORT within the scan period after
the last round. On the unfixed server about half the rounds leave the
intents (the forwarded batch is the first one when chunk 0's lane is in
the other group); with 8 rounds it fails with near certainty.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, SLOW, Mount, fuse_ok, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('publication unit forward abort: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
ROUNDS = 8
BOUND_S = 3.0 * SLOW
SIZE = 1024 * 1024
work = Path(tempfile.mkdtemp(prefix='efs-pufa-', dir=os.environ.get('TMPDIR')))
flags = work / 'flags'
flags.mkdir()
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env['EFS_FAULT_FWD_TXN_STALE'] = str(flags)
t0 = time.time()

c = Cluster(work, 'c', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    ids = [c.node_id(i) for i in (1, 2, 3)]
    m = Mount(c, 1, 'mnt')
    mounts.append(m)
    d = m.path / 'd'
    d.mkdir()
    fired_before = 0
    took = []
    for r in range(ROUNDS):
        name = d / f'f{r}'
        name.write_bytes(b'')          # the create is not part of the measured unit
        for nid in ids:
            (flags / f'stale-{nid}').write_text('1\n')
        data = payload(name.name, SIZE)
        start = time.time()
        with open(name, 'r+b') as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        took.append(round(time.time() - start, 2))
        for nid in ids:                # one forced refusal per round at most
            (flags / f'stale-{nid}').unlink(missing_ok=True)
        fired = sum(c.log(i).count('fault: EFS_FAULT_FWD_TXN_STALE') for i in (1, 2, 3))
        assert fired > fired_before, f'round {r}: no forwarded TXN_BATCH was refused (leaders?)'
        fired_before = fired
        assert name.read_bytes() == data, f'{name} differs'
    slow = [(r, s) for r, s in enumerate(took) if s > BOUND_S]
    assert not slow, f'write+fsync over {BOUND_S} s after a refused forwarded PREPARE batch: {slow} ' \
                     f'(all: {took})'
    time.sleep(8 * SLOW)              # one recovery period past the last round
    rec = [ln for i in (1, 2, 3) for ln in c.log(i).splitlines() if re.search(r'txn-recover .*-> ABORT', ln)]
    assert not rec, 'undecided PREPAREs reached the recovery scan:\n' + '\n'.join(rec[:5])
    print(f'{ROUNDS} rounds, {fired_before} forwarded PREPARE batches refused after they applied; '
          f'write+fsync max {max(took)} s <= {BOUND_S}, no txn-recover ABORT, files exact PASS')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_PUFA_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'publication unit forward abort: PASS ({time.time() - t0:.1f} s)')
