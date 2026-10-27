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
file <dir>/stale-<node id>. The two groups get different leaders
(raft-step-down when one member leads both), so the leader running a REPORT forwards the other group's
batch, and a fresh directory per round varies which group's batch is
submitted first. For each of ROUNDS fresh 1 MiB files (8
chunks: a unit spanning both groups) every member's flag is armed, the file
is written and fsynced, and the test checks: write+fsync
took under BOUND_S (the recovery scan alone takes 5 s), the file reads back
exact, the hook fired in at least half the rounds, and no member logs a
txn-recover ABORT within the scan period after the last round. On the
unfixed server a round whose first batch is the forwarded one leaves the
intents and fails EIO or over the bound (5 of 5 runs fail with the fix of
`d7a6eed7e` reverted; 6 of 6 pass with it).
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, SLOW, Mount, fuse_ok, mgmt, payload  # noqa: E402
from pub_unit_setup import split_leaders  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('publication unit forward abort: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
ROUNDS = 12
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
    # The REPORT runs at a group leader, which forwards the other group's
    # batch: the two groups must have different leaders (a member leading
    # both forwards nothing). Move group 2 off the leader of group 0.
    # Formation can precede the target's matched tail/current-term commit.
    # Wait through documented transfer refusals before starting the measured
    # publication workload; the 12 rounds and 3-second bound stay unchanged.
    place = split_leaders(c, mgmt, 30 * SLOW)
    led = set(place.values())
    via = next((i for i in (1, 2, 3) if c.node_id(i) not in led), None)
    assert via, f'every member leads a group: {place}'
    m = Mount(c, via, 'mnt')
    mounts.append(m)
    fired_before = hit = 0
    took = []
    for r in range(ROUNDS):
        # A fresh directory per round: inode numbers come from the parent's
        # shard, so files of one directory put chunk 0's lane (the first
        # batch submitted) in the same group every round.
        d = m.path / f'd{r}'
        d.mkdir()
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
        hit += fired > fired_before    # a round whose writes reported without a unit fires nothing
        fired_before = fired
        assert name.read_bytes() == data, f'{name} differs'
    assert hit >= ROUNDS // 2, f'the hook refused a forwarded batch in only {hit} of {ROUNDS} rounds'
    slow = [(r, s) for r, s in enumerate(took) if s > BOUND_S]
    assert not slow, f'write+fsync over {BOUND_S} s after a refused forwarded PREPARE batch: {slow} ' \
                     f'(all: {took})'
    time.sleep(8 * SLOW)              # one recovery period past the last round
    rec = [ln for i in (1, 2, 3) for ln in c.log(i).splitlines() if re.search(r'txn-recover .*-> ABORT', ln)]
    assert not rec, 'undecided PREPAREs reached the recovery scan:\n' + '\n'.join(rec[:5])
    print(f'{ROUNDS} rounds, {hit} with a forwarded PREPARE batch refused after it applied; '
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
