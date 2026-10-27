#!/usr/bin/env python3
"""K43: a writer whose REPORT coordinator is SIGKILLed mid-unit must not see
EBUSY/EIO with one member down.

Six fault-build efsd on loopback (tests/k33_daemons.py), one FUSE client on a
member that leads neither group. A 64 MiB private overwrite at the mount root
(its units coordinate on the group-0 leader, as in the Task D soak incident:
root-level files, inode shard on group 0). Every daemon carries
EFS_FAULT_PUB_BARRIER; arming it with "prepare" parks the next publication
unit after its PREPAREs are committed and before the DECIDE (the exact K43
moment), creating <barrier>.ready. The test then SIGKILLs the coordinator —
the group-0 leader — and disarms the barrier so the resent unit is not parked
as well.

On the base the killed unit's PREPARE intents linger until the recovery
scan's age gate (HOST_REC_AGE_NS, 5 s, plus its scan period) aborts them; the
client's resend loses its CAS against them and its 8 s REPORT budget runs out
just before: write() EBUSY / close() EIO (the K43 signature from the Task D
soak, attempt 3, event 15). On the fixed branch the abort of a lost
coordinator's intents is driven as soon as the unit is decided, the resend
clears it, and the write finishes.

Asserts (the first is the application contract; the rest pin the mechanism):
1. the writer's write/fsync/close succeed — never EBUSY/EIO with one member
   down;
2. no `txn-recover ... -> ABORT` line shows the killed unit's intents
   reaching the recovery age gate (they are resolved by the finisher);
3. the final content reads back byte-exact through a fresh mount.

Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import os, re, shutil, signal, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, FUSE, SLOW, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT, FUSE):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K43 coordinator kill: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)

# 16 MiB per phase keeps three multi-unit writes meaningful (each unit still
# covers dozens of chunk records) while the pool-time disk cost stays in
# budget; K43_SIZE=64MiB reproduces the incident's exact volume.
SIZE = int(os.environ.get('K43_SIZE', str(16 << 20)))
PARK_DEADLINE = 60 * SLOW
WRITE_DEADLINE = 120 * SLOW

work = Path(tempfile.mkdtemp(prefix='efs-k43-', dir=os.environ.get('TMPDIR')))
barrier = work / 'pub-barrier'
np_marker = work / 'pub-unit-np'
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env['EFS_TRANSPORT'] = 'tcp'
env['EFS_FAULT_PUB_BARRIER'] = str(barrier)
env['EFS_FAULT_PUB_UNIT_NP_FILE'] = str(np_marker)

t0 = time.time()
c = None
mounts = []
write_rc = []


def overwrite(path, data, create=True):
    """write+fsync+close in one go; record any error with its stage.

    create=False is an in-place overwrite (no O_TRUNC): the inode generation
    stays, so the rewrite's publication stages land on the same keys as a
    killed predecessor's — the K43 contact. O_TRUNC would bump the
    generation and sidestep it entirely."""
    op = 'write'
    fd = None
    try:
        fd = os.open(path, os.O_WRONLY | (os.O_CREAT | os.O_TRUNC if create else 0))
        view = memoryview(data)
        while view:
            view = view[os.write(fd, view):]
        op = 'fsync'
        os.fsync(fd)
        op = 'close'
        os.close(fd)   # releases the fd even when it reports an error
        fd = None
        write_rc.append('ok')
    except OSError as e:
        write_rc.append(f'{op}: {e}')
        if fd is not None and op != 'close':
            try:
                os.close(fd)
            except OSError:
                pass


try:
    c = Cluster(work, 'c', [1, 2, 3, 4, 5, 6], env)
    c.start(1)
    for i in range(2, 7):
        c.start(i, join=1)
    for i in range(1, 7):
        c.wait_log(i, r'formed: 6 members', timeout=120)
    c.wait_members(list(range(1, 7)), 6)
    place = c.leader_placement(1)
    # The client routes a REPORT to the first dual-group voter (both metadata
    # groups share nodes 2 and 3 with the compiled-in mapping). The test's
    # coordinator is that host; make it the group-0 leader (the incident's
    # coordinator led group 0), so the SIGKILL forces a leader change on the
    # coordinating group mid-unit, then mount on another member.
    # The soak's coordinator was killed while leading the group the unit
    # decided on; its death left that group leaderless for an election
    # window, which is what the resend's NOT_PRIMARY came from. Put node 2
    # (the first dual-group voter the client routes REPORTs to) in the lead
    # of BOTH groups, so the kill opens that window on both.
    for g in (0, 2):
        if place[g] != c.node_id(2):
            li = next(i for i in range(1, 7) if c.node_id(i) == place[g])
            rc, out = mgmt(env, 'raft-step-down', c.ep(li), str(g), str(c.node_id(2)))
            assert rc == 0, out
            end = time.time() + 30 * SLOW
            while place[g] != c.node_id(2) and time.time() < end:
                time.sleep(0.5)
                place = c.leader_placement(1)
        assert place[g] == c.node_id(2), f'group {g} would not move to node 2: {place}'
    kill_i = 2
    via = next(i for i in (1, 3, 4, 5, 6))
    print(f'[k43] group leaders: {place}; mount on node {via}, '
          f'group-0 leader node {kill_i} is the coordinator to kill', flush=True)

    m = Mount(c, via, 'mnt')
    mounts.append(m)
    target = m.path / 'k43-target'

    base = payload('k43-base', SIZE)
    print(f'[k43] baseline write {SIZE >> 20} MiB...', flush=True)
    overwrite(target, base)
    assert write_rc == ['ok'], f'baseline write failed: {write_rc}'
    assert target.read_bytes() == base
    print(f'[k43] baseline exact; {int(time.time() - t0)} s', flush=True)

    # arm the barrier: the next unit parks after its PREPAREs, undecided
    barrier.write_text('prepare\n')
    new = payload('k43-overwrite', SIZE)
    wt = threading.Thread(target=overwrite, args=(target, new), kwargs={'create': False}, daemon=True)
    wt.start()
    end = time.time() + PARK_DEADLINE
    while time.time() < end and not (work / 'pub-barrier.ready').exists():
        time.sleep(0.02)
    assert (work / 'pub-barrier.ready').exists(), \
        'no publication unit parked after its PREPAREs within the deadline'
    coord = next(i for i in range(1, 7)
                 if 'publication unit parked at prepare' in c.log(i))
    assert coord == kill_i, \
        f'unit parked on node {coord}, not the connected group-0 leader {kill_i}'
    print(f'[k43] a unit is parked after its PREPAREs on node {coord} '
          f'(the group-0 leader); {int(time.time() - t0)} s', flush=True)

    # SIGKILL the coordinator between the PREPARE batch and the decision;
    # disarm so the resent unit (new leader) is not parked too
    kp = c.procs[kill_i]
    os.kill(kp.pid, signal.SIGKILL)
    kp.wait(timeout=30 * SLOW)
    assert kp.poll() is not None, 'the coordinator survived SIGKILL'
    c.procs.pop(kill_i)   # stop_all skips it
    t_kill = time.time()
    # the failover host cannot admit the resent unit at once (the incident's
    # new-leader window): the next unit answers NOT_PRIMARY and its abort
    # decides the killed coordinator's parked transaction; the finisher then
    # parks between the abort's decision and its RESOLVEs ("abort-resolve"),
    # holding the intents — the incident's decision-to-resolution window
    np_marker.write_text('1\n')
    barrier.write_text('abort-resolve\n')
    print(f'[k43] coordinator (node {kill_i}, pid {kp.pid}) SIGKILLed mid-unit', flush=True)

    wt.join(timeout=WRITE_DEADLINE)
    assert not wt.is_alive(), f'overwrite still running {WRITE_DEADLINE} s after the kill'
    took = time.time() - t_kill
    assert write_rc[-1] == 'ok', \
        f'K43 signature: the writer saw {write_rc[-1]} with the coordinator killed mid-unit'
    print(f'[k43] overwrite finished {took:.1f} s after the kill, no application error',
          flush=True)

    # The discriminator: while the abort's RESOLVEs are still parked (the
    # incident's decision-to-resolution window), a SECOND client overwrites
    # the same file in place: its units' PREPAREs meet the killed unit's
    # still-staged intents on the same chunk keys. On the base they answer
    # BUSY until the REPORT budget runs out: EBUSY/EIO. The fix reaps an
    # ABORT-decided stage on contact, so the write proceeds.
    via2 = next(i for i in range(1, 7) if i not in (kill_i, via))
    mb = Mount(c, via2, 'mnt-b')
    mounts.append(mb)
    newer = payload('k43-third', SIZE)
    write_rc.clear()
    wt3 = threading.Thread(target=overwrite, args=(mb.path / 'k43-target', newer),
                           kwargs={'create': False}, daemon=True)
    wt3.start()
    wt3.join(timeout=WRITE_DEADLINE)
    barrier.unlink(missing_ok=True)   # release the parked finisher
    assert not wt3.is_alive(), 'second client rewrite still running past the deadline'
    assert write_rc[-1] == 'ok', \
        f'K43 signature against the held resolve: the second client saw {write_rc[-1]}'
    print('[k43] second client rewrote the shared file past the held resolve: '
          'no application error', flush=True)
    new = newer  # the final content for the read-back below

    # mechanism: the killed unit's intents must be resolved by the finisher
    # (or reaped on contact), never by the recovery age gate
    rec = []
    reaped = 0
    for i in range(1, 7):
        if i == kill_i or not c.alive(i):
            continue
        for ln in c.log(i).splitlines():
            mm = re.search(r'txn-recover .*age=([0-9.]+)s -> ABORT', ln)
            if mm:
                rec.append((i, mm.group(1)))
            if 'reaped a publication stage on contact' in ln:
                reaped += 1
    assert not rec, f'intents reached the recovery age gate: {rec}'
    print(f'[k43] no txn-recover age-gate abort: the intents were resolved directly '
          f'(contact reaps: {reaped})', flush=True)

    # fresh-reader proof: the final content, byte-exact
    m2 = Mount(c, next(i for i in range(1, 7) if i not in (kill_i, via, via2)), 'mnt-verify')
    mounts.append(m2)
    got = (m2.path / 'k43-target').read_bytes()
    assert got == new, 'fresh reader: content differs after the coordinator kill'
    print('[k43] fresh reader: 64 MiB exact', flush=True)
    print(f'[k43] coordinator kill mid-REPORT: write retried past the lost unit, '
          f'no EBUSY/EIO, data exact PASS ({time.time() - t0:.1f} s)', flush=True)
except Exception as e:
    print('[k43] FAIL:', e, flush=True)
    raise
finally:
    barrier.unlink(missing_ok=True)
    for mt in mounts:
        mt.close()
    if c:
        c.stop_all()
    import sys as _sys
    if _sys.exc_info()[0] is None and not os.environ.get('EFS_K43_KEEP'):
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f'[k43] work dir kept: {work}')
