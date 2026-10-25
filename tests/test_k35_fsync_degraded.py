#!/usr/bin/env python3
"""K35 (D42, F22): fsync and fdatasync with a member down, only with proof.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and efs-fuse; private stores under TMPDIR, free ports (never 17xxx). One
mount through node 1. Large files are 300000 B (several chunks), small
files 1 B to 16 KiB (packed by the server packer, 500 ms minimum age).

Three members:
 1. All up: write, close, fsync: no degraded mark (`Protection: full`).
 2. Packed small files: written and packed with all up; node 3 stops;
    fsync of each succeeds (the packed copy replaced the objects).
 3. Member down after the write, before fsync: large files written and
    closed with all up, then fsync with node 3 stopped succeeds, the client
    marks each stripe degraded before returning (client log), and `status`
    shows the count and `Protection: DEGRADED`.
 4. Member down before the write: write and fsync with node 3 still down
    succeed (W154 degraded acknowledgement, published marks).
 5. Two members down: a file written with node 3 down, then node 2 stops:
    fsync and fdatasync are EIO and the client log names the stripe;
    nodes 2 and 3 return (orderly restarts keep their verifiers): the same
    fsync succeeds.
 6. W154: the rebuild on return clears every mark (count 0, `Protection:
    full`), every file reads back exactly.
Four, then five members (additions through K33):
 7. An unrelated member down (node 4 of 4): writes and fsync succeed and
    leave no degraded mark: fragments go to the available members. The
    interleaving that broke this is forced: node 3 is first marked "away"
    on the client (a write while it is stopped fails its fragment hard and
    is placed around on node 4, no mark), returns, and node 4 stops while
    that mark is live (the product's 10 s window: efs-fuse is a normal
    build, so EFS_FAULT_AWAY_MARK_MS does not exist there and this case
    runs timed; node 4 stops a few seconds after node 3's return). The
    client must then place around node 4 onto node 3, the away member, rather than
    acknowledge on two fragments (it did, until the fix: 2 of 10 gate
    runs, whenever node 4 joined and stopped within 10 s of node 3's last
    hard failure).
 7b. K39: the leader of metadata group 2 stops (never node 1 at four
    members) while a writer keeps writing and syncing: no EIO. The client's put-ticket ADMIT to
    the stopping leader lost its exchange and was NET at once, so the
    write was EIO; it now re-picks another voter within the budget.
 8. Five members, node 2 down: the same, no degraded mark.
 9. A member that dies during COMMIT (node 5, EFS_FAULT_MEMBER_EXIT=
    data-commit): fsync succeeds with the stripes on node 5 marked
    degraded; node 5 ended with status 86 at the hook. It is not started
    again (an unclean stop leaves its store recovery-uncertain, K36).
Every file written in the run reads back exactly at the end.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import errno, os, re, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload, voters_ok  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K35 fsync degraded: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k35-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
SMALL = (1, 512, 4096, 16384)
LARGE = 300000
files = {}
t0 = time.time()


def write(m, name, size):
    (m.path / name).write_bytes(payload(name, size))
    files[name] = size


def fsync(m, name, data=False):
    fd = os.open(m.path / name, os.O_RDWR)
    try:
        (os.fdatasync if data else os.fsync)(fd)
    finally:
        os.close(fd)


def status(c, i):
    rc, out = mgmt(c.env, 'status', c.ep(i))
    return out


def degraded(c, i):
    m = re.search(r'^Degraded chunks: (\d+) ', status(c, i), re.M)
    return int(m.group(1)) if m else None


def wait_degraded(c, i, pred, what, timeout=90):
    end, got = time.time() + timeout, None
    while time.time() < end:
        got = degraded(c, i)
        if got is not None and pred(got):
            return got
        time.sleep(0.5)
    raise AssertionError(f'{what}: degraded count {got}\n{status(c, i)}')


def packed_total(c):
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


def verify(m, what):
    for name, size in files.items():
        got = (m.path / name).read_bytes()
        assert got == payload(name, size), f'{what}: {name} differs ({len(got)} of {size} B)'


def client_log(m):
    return m.logp.read_text(errors='replace')


def settle(m, tag, timeout=90):
    """Until a fresh write is a full stripe again: a member that returned is
    in the client's down cooldown until its probe reconnects (about 1 s)."""
    end, k = time.time() + timeout, 0
    while True:
        before = client_log(m).count('degraded acknowledgement')
        write(m, f'settle-{tag}-{k}', LARGE)
        fsync(m, f'settle-{tag}-{k}')
        if client_log(m).count('degraded acknowledgement') == before:
            return
        assert time.time() < end, client_log(m)[-3000:]
        k += 1
        time.sleep(0.5)


def up(c, i, n):
    c.wait_log(i, rf'raft-host: up raft_id={i - 1} n={n} formed', timeout=60)


c = Cluster(work, 'k', [1, 2, 3, 4, 5], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt')
    mounts.append(a)

    # 1. All up.
    write(a, 'up-0', LARGE)
    fsync(a, 'up-0')
    wait_degraded(c, 1, lambda n: n == 0, 'all up')
    assert 'Protection: full' in status(c, 1), status(c, 1)
    print('1. all up: fsync, no degraded mark PASS')

    # 2. Packed small files, then a member down.
    base = packed_total(c)
    for k, size in enumerate(SMALL):
        write(a, f'small-{k}', size)
    end = time.time() + 60
    while packed_total(c) < base + len(SMALL):
        assert time.time() < end, f'packer moved {packed_total(c) - base} of {len(SMALL)}'
        time.sleep(0.2)
    c.stop(3)
    for k in range(len(SMALL)):
        fsync(a, f'small-{k}')
    print('2. packed small files: fsync with node 3 down PASS')

    # 3. Member down after the write, before fsync.
    c.restart(3)
    up(c, 3, 3)
    c.wait_members([1, 2, 3], 3)
    settle(a, 3)
    for k in range(3):
        write(a, f'after-{k}', LARGE)
    c.stop(3)
    before = client_log(a).count('marked degraded')
    for k in range(3):
        fsync(a, f'after-{k}', data=k == 1)
    marked = client_log(a).count('marked degraded') - before
    assert marked >= 3, client_log(a)[-3000:]
    n3 = wait_degraded(c, 1, lambda n: n >= marked, 'down after the write')
    st = status(c, 1)
    assert 'Protection: DEGRADED' in st and 'Cluster state: DEGRADED' in st, st
    print(f'3. member down after the write: fsync/fdatasync OK, {marked} stripes marked, '
          f'status shows {n3} degraded PASS')

    # 4. Member down before the write.
    for k in range(2):
        write(a, f'before-{k}', LARGE)
        fsync(a, f'before-{k}')
    n4 = wait_degraded(c, 1, lambda n: n > n3, 'down before the write')
    print(f'4. member down before the write: fsync OK, {n4} degraded PASS')

    # 5. Two members down.
    write(a, 'two-0', LARGE)
    # Opened first: with two of three nodes down no metadata group has a
    # quorum, so a new open is refused (EBUSY); fsync of the open file is
    # what is tested.
    fd = os.open(a.path / 'two-0', os.O_RDWR)
    c.stop(2)
    before = client_log(a).count('need 2')
    for sync in (os.fsync, os.fdatasync):
        try:
            sync(fd)
            raise AssertionError(f'{sync.__name__}: succeeded without proof')
        except OSError as e:
            assert e.errno == errno.EIO, e
    assert client_log(a).count('need 2') > before, client_log(a)[-3000:]
    assert re.search(r'fsync ino=\d+ chunk=\d+ gen=[0-9a-f]+ nodes=\d+,\d+,\d+ '
                     r'fragments=\S+: 1 of 3 durable; need 2', client_log(a)), client_log(a)[-3000:]
    c.restart(2)
    c.restart(3)
    for i in (2, 3):
        up(c, i, 3)
    c.wait_members([1, 2, 3], 3)
    end = time.time() + 60
    while True:
        try:
            os.fsync(fd)
            break
        except OSError:
            assert time.time() < end, client_log(a)[-3000:]
            time.sleep(1)
    os.close(fd)
    settle(a, 5)
    print('5. two members down: fsync and fdatasync EIO with the stripe logged; OK once they '
          'return PASS')

    # 6. W154 rebuild on return clears every mark.
    wait_degraded(c, 1, lambda n: n == 0, 'rebuild on return', timeout=180)
    assert 'Protection: full' in status(c, 1), status(c, 1)
    assert sum(c.log(i).count('raft-host: rebuilt ino=') for i in (1, 2, 3)) >= n4, \
        'rebuilds not logged'
    verify(a, 'after the rebuild')
    print('6. W154: rebuild on return cleared every mark, files exact PASS')

    # 7. Four members, the unrelated one down.
    c.start(4, join=2)
    up(c, 4, 4)
    c.wait_members([1, 2, 3, 4], 4)
    c.wait_voters([1, 2, 3, 4], lambda i, v: voters_ok(i, v, 4))
    write(a, 'four-warm', LARGE)
    fsync(a, 'four-warm')
    # Node 3 away on the client: one single-chunk write whose stripe has
    # node 3 fails that fragment hard while node 3 is stopped and is placed
    # around on node 4 (`placing again`; four members: no mark). One chunk
    # keeps node 3 below the four hard failures that would mark it down
    # for 30 s (EFS_NODE_DOWN_FAILS, EFS_NODE_DOWN_MS): a down member is no
    # spare either, by design; what this case forces is the away mark
    # alone. Later stripes are routed around the away member before any
    # attempt, so they add no failures.
    c.stop(3)
    placed = re.compile(r'put_fragments ino=\d+ chunk=\d+: member\(s\) (\d),(\d),(\d) unreachable')
    for k in range(24):
        write(a, f'away-{k}', 65536)
        fsync(a, f'away-{k}')
        if any('3' in m.groups() for m in placed.finditer(client_log(a))):
            break
    else:
        raise AssertionError('no stripe of 24 single-chunk files had node 3: ' + client_log(a)[-3000:])
    wait_degraded(c, 1, lambda n: n == 0, 'four members, node 3 down')
    marks = client_log(a).count('degraded acknowledgement')
    c.restart(3)
    up(c, 3, 4)
    c.wait_members([1, 2, 3, 4], 4)
    # Node 4 stops while node 3's away mark is live: the only spare is
    # the away member.
    c.stop(4)
    for k in range(6):
        write(a, f'four-{k}', LARGE)
        fsync(a, f'four-{k}')
    time.sleep(3)
    wait_degraded(c, 1, lambda n: n == 0, 'four members, node 4 down')
    assert client_log(a).count('degraded acknowledgement') == marks, client_log(a)[-3000:]
    assert re.search(r'placing again on (?:3,|\d,3,|\d,\d,3)', client_log(a)), client_log(a)[-3000:]
    print('7. four members, node 4 down while node 3 is away-marked: fsync OK, fragments placed '
          'around on node 3, no mark PASS')
    c.restart(4)
    up(c, 4, 4)
    c.wait_members([1, 2, 3, 4], 4)
    settle(a, 4)

    # 7b. K39: the leader of metadata group 2 (voters 2, 3, 4 at four
    # members, so never node 1) stops while a writer keeps writing and
    # syncing. Tickets of its lane shards are in flight to it: the client's
    # ADMIT lost its exchange with the stopping leader and returned NET at
    # once, the write EIO (2 of 3 merge-gate runs, cases 7 and 8).
    led = c.leader_placement(1)
    lead = led[2]
    assert lead != 1, led
    errs, stop_writer = [], threading.Event()
    log_at = len(client_log(a))

    def writer():
        k = 0
        try:
            while not stop_writer.is_set():
                write(a, f'lead-{k}', LARGE)
                fsync(a, f'lead-{k}')
                k += 1
        except OSError as e:
            errs.append(f'lead-{k}: {e!r}')

    t = threading.Thread(target=writer)
    t.start()
    time.sleep(1)
    c.stop(lead)
    time.sleep(4)
    stop_writer.set()
    t.join(timeout=120)
    assert not t.is_alive() and not errs, (errs, client_log(a)[-3000:])
    # Stripes written before the stop and synced after it are marked
    # degraded (case 3's rule), and one acknowledged while the leader was
    # going away may be published degraded. Case 8 stops another member,
    # so they must be rebuilt on the leader's return first. The leaders
    # report the count periodically: wait until it includes every stripe
    # the client named (none is rebuilt while the leader is down), then
    # for zero after the return.
    named = set(re.findall(r'ino=(\d+) chunk=(\d+) gen=([0-9a-f]+) nodes=[\d,]+(?: fragments=\S+)?: '
                           r'(?:degraded acknowledgement|2 of 3 durable; marked degraded)',
                           client_log(a)[log_at:]))
    if named:
        wait_degraded(c, 1, lambda n: n >= len(named), f'7b: {len(named)} stripes counted')
    print(f'7b. K39: group 2 leader (node {lead}) stopped under a writer: writes and fsync OK, '
          f'{len(named)} stripes degraded PASS')
    c.restart(lead)
    up(c, lead, 4)
    c.wait_members([1, 2, 3, 4], 4)
    settle(a, f'7b-{lead}')
    wait_degraded(c, 1, lambda n: n == 0, '7b rebuild on return', timeout=180)

    # 8. Five members, node 2 down.
    c.start(5, join=3)
    up(c, 5, 5)
    c.wait_members([1, 2, 3, 4, 5], 5)
    c.wait_voters([1, 2, 3, 4, 5], lambda i, v: voters_ok(i, v, 5))
    c.stop(2)
    for k in range(6):
        write(a, f'five-{k}', LARGE)
        fsync(a, f'five-{k}')
    time.sleep(3)
    wait_degraded(c, 1, lambda n: n == 0, 'five members, node 2 down')
    c.restart(2)
    up(c, 2, 5)
    c.wait_members([1, 2, 3, 4, 5], 5)
    settle(a, 2)
    print('8. five members, node 2 down: fsync OK, no mark PASS')

    # 9. A member that dies during COMMIT.
    c.stop(5)
    c.restart(5, extra_env={'EFS_FAULT_MEMBER_EXIT': 'data-commit'})
    up(c, 5, 5)
    c.wait_members([1, 2, 3, 4, 5], 5)
    for k in range(8):
        write(a, f'commit-{k}', LARGE)
    before = client_log(a).count('marked degraded')
    for k in range(8):
        fsync(a, f'commit-{k}')
    assert c.wait_exit(5, timeout=30) == 86
    assert 'EFS_FAULT_MEMBER_EXIT at data-commit' in c.last_run(5), c.last_run(5)[-2000:]
    marked = client_log(a).count('marked degraded') - before
    assert marked >= 1, client_log(a)[-3000:]
    wait_degraded(c, 1, lambda n: n >= marked, 'died during COMMIT')
    print(f'9. member died during COMMIT: fsync OK, {marked} stripes marked degraded PASS')
    verify(a, 'final')
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K35_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'K35 fsync with a member down: proof per stripe, degraded marks, EIO without proof, '
      f'rebuild on return, placement on 4 and 5 members, death during COMMIT: PASS '
      f'({time.time() - t0:.1f} s, {len(files)} files exact)')
