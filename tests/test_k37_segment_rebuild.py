#!/usr/bin/env python3
"""K37: rebuild on return for packed segments.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and efs-fuse; private stores under TMPDIR, free ports (never 17xxx). Three
members, one mount through node 1, small files (1 B to 16 KiB) packed by the
server packer (500 ms minimum age). Fault hooks, all toggled by files in the
work directory so each step is deterministic:
  EFS_FAULT_PACK_PLACE_DOWN=3       segments are placed on node 3 even while
                                    it is down (the window before a stopped
                                    member is detected): its PUT fails;
  EFS_FAULT_SEGMENT_PUT_REFUSE=DIR  node N refuses segment PUTs while
                                    DIR/refuse-N exists;
  EFS_FAULT_SEGMENT_PUT_TRANSIENT=DIR  node N refuses one segment PUT
                                    NOT_PRIMARY while DIR/transient-N exists
                                    (consumed);
  EFS_FAULT_SEGMENT_REBUILD_HOLD=F  marks wait while F exists.

 1. Baseline: two batches packed with all members up; no segment mark,
    `Protection: full`.
 1b. A transient refusal is not a missing fragment: node 3 refuses one
    segment PUT NOT_PRIMARY (as a follower under load refuses the
    linearizable read of the object admitted a moment ago); the packer
    sends the fragment again (`resent after rc=-15: rc=0`), the segment
    has three fragments, no `written degraded`, no put error (D4's gate
    saw one such refusal per failing run: 1 of 10 parallel gate runs).
 2. Fragments deleted and torn while their member is down: node 2 stops, one
    of its segment fragments is deleted and another truncated; on its return
    the group leaders' audit marks both (missing, torn), the rebuild restores
    them (files present, full length, verified), every file reads back.
 3. Written with a member down: node 3 stopped, new files are packed into
    segments written with two fragments, each marked before it holds files
    (`written degraded`, `Degraded segments: N`); the files read back with
    node 3 down. Node 3 returns with the rebuild held: every member is up and
    `Cluster state: DEGRADED`, `Protection: DEGRADED`. Released: the marks
    clear, node 3 holds every rebuilt fragment, `Protection: full`.
 4. Compaction with a member down: half of the packed files are removed,
    node 3 stops, `efs-mgmt pack-compact` copies the survivors into new
    segments written degraded and marked; node 3 returns: rebuilt, exact.
 5. Never on fewer than two: node 3 down and node 2 refusing segment PUTs:
    the packer writes only one fragment, cancels, marks nothing and switches
    nothing; the files stay unpacked and read back.
 6. Single-member outages after the rebuilds: each member in turn stops, a
    fresh mount through another member reads every file exactly.
 7. A second loss before the rebuild: node 3 down, a new segment is written
    degraded, then its fragment on node 2 is deleted. When node 3 returns the
    rebuild finds the source lost: the segment is reported unrecoverable
    (`unrecoverable=1`, `Protection: LOST`, `Cluster state: DEGRADED` with
    all members up), never healed; its files read EIO, every other file reads
    back; removing its files retires the mark (`Protection: full`).
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import errno, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K37 segment rebuild: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k37-', dir=os.environ.get('TMPDIR')))
flags = work / 'flags'
flags.mkdir()
hold = flags / 'hold'
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200',
           EFS_PACK_COMPACT_IDLE_S='1000000',
           # the daily compaction window (2 h) starts 12 h from now
           EFS_PACK_COMPACT_DAILY_START_MIN=str((time.gmtime().tm_hour * 60 + time.gmtime().tm_min
                                                 + 720) % 1440),
           EFS_FAULT_PACK_PLACE_DOWN='3', EFS_FAULT_SEGMENT_PUT_REFUSE=str(flags),
           EFS_FAULT_SEGMENT_PUT_TRANSIENT=str(flags),
           EFS_FAULT_SEGMENT_REBUILD_HOLD=str(hold))
SIZES = (1, 300, 512, 4096, 5000, 9000, 12000, 16384)
files = {}
lost = set()
t0 = time.time()


def write(m, name, size):
    (m.path / name).write_bytes(payload(name, size))
    files[name] = size


def status(c, i):
    return mgmt(c.env, 'status', c.ep(i))[1]


def segcount(c, i):
    m = re.search(r'^Degraded segments: (\d+) .*?unrecoverable=(\d+)', status(c, i), re.M)
    return (int(m.group(1)), int(m.group(2))) if m else None


def wait_segs(c, i, pred, what, timeout=120):
    end, got = time.time() + timeout, None
    while time.time() < end:
        got = segcount(c, i)
        if got is not None and pred(*got):
            return got
        time.sleep(0.5)
    raise AssertionError(f'{what}: degraded segments {got}\n{status(c, i)}')


def logs(c, ids=(1, 2, 3)):
    return ''.join(c.log(i) for i in ids if (c.dir / f'n{i}.log').exists())


def count(c, pattern):
    return len(re.findall(pattern, logs(c)))


def packer_stats(c):
    """Node-wide packer counters since each daemon's start, summed over the
    running members: (files_packed, cancelled, put_errors). Compare only
    within a step that restarts no member."""
    rc, out = mgmt(c.env, 'pack-compact', c.ep(next(i for i in sorted(c.procs) if c.alive(i))), '0')
    got = re.findall(r'files_packed=(\d+) cancelled=(\d+) .*?put_errors=(\d+)', out)
    assert got, out
    return tuple(sum(int(g[k]) for g in got) for k in range(3))


def packed_total(c):
    return packer_stats(c)[0]


def wait_packed(c, base, n, what, timeout=120):
    end = time.time() + timeout
    while packed_total(c) < base + n:
        assert time.time() < end, f'{what}: packer moved {packed_total(c) - base} of {n}\n{status(c, 1)}'
        time.sleep(0.3)


def seg_files(c, i):
    """{(group, id, gen, fi): path} of node i's segment fragment files."""
    out = {}
    for p in (c.dir / f'n{i}' / 'data' / 'segments').glob('g*/*/*'):
        m = re.match(r'^([0-9a-f]{16})\.([0-9a-f]+)\.(\d)$', p.name)
        if m:
            out[(int(p.parent.parent.name[1:]), int(m.group(1), 16), int(m.group(2), 16),
                 int(m.group(3)))] = p
    return out


def seg_path(c, i, group, sid, gen, fi):
    return (c.dir / f'n{i}' / 'data' / 'segments' / f'g{group}' / f'{sid & 0xfff:03x}' /
            f'{sid:016x}.{gen:x}.{fi}')


def degraded_writes(c, since=()):
    """(group, id, gen, fragment, node) of each `written degraded` line of
    every member's log, minus those in `since` (an earlier result). The logs
    are concatenated in node order, so a position in the combined list is
    not stable: a packer that moved to another leader after a step-down
    appends its lines before the other members' older ones, and slicing by
    count then names an old, already rebuilt segment as the new one."""
    seen = set(since)
    return [e for e in ((int(g), int(i, 16), int(n, 16), int(f), int(node)) for g, i, n, f, node in
                        re.findall(r'packer: segment g(\d+) id=([0-9a-f]+) gen=([0-9a-f]+) written degraded: '
                                   r'fragment (\d+) for node (\d+)', logs(c)))
            if e not in seen]


def verify(c, node, what, skip=()):
    m = Mount(c, node, f'v{int(time.time() * 1000)}')
    try:
        for name, size in files.items():
            if name in lost or name in skip:
                continue
            got = (m.path / name).read_bytes()
            assert got == payload(name, size), f'{what}: {name} differs ({len(got)} of {size} B)'
    finally:
        m.close()


def up(c, i):
    c.wait_log(i, r'raft-host: up raft_id=\d+ n=3 formed', timeout=60)
    c.wait_members([1, 2, 3], 3)


def wait_up_status(c, timeout=60):
    end = time.time() + timeout
    while 'Cluster state: OK (3 up, 0 down)' not in status(c, 1):
        assert time.time() < end, status(c, 1)
        time.sleep(0.5)


def batch(m, tag, n=len(SIZES)):
    for k in range(n):
        write(m, f'{tag}-{k}', SIZES[k % len(SIZES)])


c = Cluster(work, 'k', [1, 2, 3], env)
mounts = []
try:
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt')
    mounts.append(a)

    # 1. Baseline.
    base = packed_total(c)
    batch(a, 'base0')
    wait_packed(c, base, len(SIZES), 'baseline 0')
    batch(a, 'base1')
    wait_packed(c, base, 2 * len(SIZES), 'baseline 1')
    wait_segs(c, 1, lambda n, u: n == 0, 'baseline')
    assert 'Protection: full' in status(c, 1), status(c, 1)
    assert not degraded_writes(c)
    n2 = seg_files(c, 2)
    assert len(n2) >= 2, n2
    print(f'1. baseline: {packed_total(c) - base} files packed, {len(n2)} segments, no mark PASS')

    # 1b. One transient refusal from node 3: resent, never written degraded.
    put_errors0 = packer_stats(c)[2]
    (flags / 'transient-3').write_text('')
    base = packed_total(c)
    batch(a, 'transient', 4)
    wait_packed(c, base, 4, 'transient')
    assert not (flags / 'transient-3').exists(), 'the refusal never fired'
    assert count(c, r'fault: EFS_FAULT_SEGMENT_PUT_TRANSIENT refused segment') == 1
    assert count(c, r'packer: segment g\d+ id=[0-9a-f]+ gen=[0-9a-f]+ fragment \d for node 3 resent '
                    r'after rc=-15: rc=0') == 1, logs(c)[-3000:]
    assert not degraded_writes(c), degraded_writes(c)
    assert packer_stats(c)[2] == put_errors0, packer_stats(c)
    wait_segs(c, 1, lambda n, u: n == 0, 'after the transient refusal')
    n3 = len(seg_files(c, 3))
    assert n3 == len(seg_files(c, 1)) == len(seg_files(c, 2)), (n3, len(seg_files(c, 1)), len(seg_files(c, 2)))
    print('1b. one segment PUT refused NOT_PRIMARY by node 3: resent, three fragments, no mark, '
          'no put error PASS')

    # 2. Deleted and torn while the member is down.
    c.stop(2)
    (gone, gone_p), (torn, torn_p) = sorted(n2.items())[:2]
    gone_p.unlink()
    with open(torn_p, 'r+b') as f:
        f.truncate(4096)
    marked0, rebuilt0 = count(c, r'fragment \d+ (?:missing|torn or bad); marked degraded'), \
        count(c, r'segment-rebuild: rebuilt g')
    c.restart(2)
    up(c, 2)
    end = time.time() + 120
    while count(c, r'segment-rebuild: rebuilt g') < rebuilt0 + 2:
        assert time.time() < end, logs(c)[-4000:]
        time.sleep(0.5)
    wait_segs(c, 1, lambda n, u: n == 0, 'return without fragments')
    assert count(c, r'audit node 2 group \d+: g\d+ id=[0-9a-f]+ gen=[0-9a-f]+ fragment \d+ missing; '
                    r'marked degraded') >= 1, logs(c)[-4000:]
    assert count(c, r'audit node 2 group \d+: g\d+ id=[0-9a-f]+ gen=[0-9a-f]+ fragment \d+ torn or bad; '
                    r'marked degraded') >= 1, logs(c)[-4000:]
    assert count(c, r'fragment \d+ (?:missing|torn or bad); marked degraded') - marked0 == 2
    for p in (gone_p, torn_p):
        assert p.exists() and p.stat().st_size > 4096, p
    assert 'Protection: full' in status(c, 1), status(c, 1)
    verify(c, 3, 'after the return of node 2')
    print('2. fragment deleted and fragment torn while node 2 was down: audited, marked, rebuilt, '
          'exact PASS')

    # 3. Written with a member down.
    wait_up_status(c)
    c.stop(3)
    seen = degraded_writes(c)
    base = packed_total(c)
    batch(a, 'down')
    wait_packed(c, base, len(SIZES), 'member down')
    dw = degraded_writes(c, seen)
    assert dw and all(node == 3 for *_, node in dw), dw
    n3, _ = wait_segs(c, 1, lambda n, u: n >= len(dw) and not u, 'written degraded')
    st = status(c, 1)
    assert 'Cluster state: DEGRADED' in st and 'Protection: DEGRADED' in st, st
    for name in [f'down-{k}' for k in range(len(SIZES))]:
        assert (a.path / name).read_bytes() == payload(name, files[name]), name
    hold.touch()
    c.restart(3)
    up(c, 3)
    time.sleep(3)
    st = status(c, 1)
    assert segcount(c, 1)[0] >= len(dw), st
    assert 'Cluster state: DEGRADED (3 up, 0 down)' in st and 'Protection: DEGRADED' in st, st
    hold.unlink()
    wait_segs(c, 1, lambda n, u: n == 0, 'rebuild of segments written degraded')
    for g, sid, gen, fi, node in dw:
        p = seg_path(c, 3, g, sid, gen, fi)
        assert p.exists() and p.stat().st_size > 4096, p
    st = status(c, 1)
    assert 'Protection: full' in st and 'Cluster state: OK' in st, st
    verify(c, 2, 'after the return of node 3')
    print(f'3. written with node 3 down: {len(dw)} segments written degraded and marked '
          f'(status {n3}), DEGRADED with all up while held, rebuilt on node 3, exact PASS')

    # 4. Compaction with a member down.
    for k in range(0, len(SIZES), 2):
        for tag in ('base0', 'base1', 'down'):
            os.unlink(a.path / f'{tag}-{k}')
            files.pop(f'{tag}-{k}')
    time.sleep(2)
    wait_up_status(c)
    c.stop(3)
    seen = degraded_writes(c)
    compacted = count(c, r'packer: compact')
    for i in (1, 2):
        rc, out = mgmt(c.env, 'pack-compact', c.ep(i), '64')
        assert rc == 0, out
    end = time.time() + 120
    while len(degraded_writes(c, seen)) == 0:
        assert time.time() < end, logs(c)[-4000:]
        time.sleep(0.5)
    dw = degraded_writes(c, seen)
    wait_segs(c, 1, lambda n, u: n >= 1, 'compaction written degraded')
    for name, size in files.items():
        assert (a.path / name).read_bytes() == payload(name, size), name
    c.restart(3)
    up(c, 3)
    wait_segs(c, 1, lambda n, u: n == 0, 'rebuild after compaction')
    for g, sid, gen, fi, node in dw:
        assert seg_path(c, 3, g, sid, gen, fi).exists()
    verify(c, 2, 'after compaction')
    print(f'4. compaction with node 3 down: {len(dw)} output segments written degraded, marked, '
          f'rebuilt, exact PASS')

    # 5. Never on fewer than two durable fragments.
    wait_up_status(c)
    c.stop(3)
    (flags / 'refuse-2').touch()
    seen = degraded_writes(c)
    base, cancelled, put_errors = packer_stats(c)
    batch(a, 'one', 4)
    end = time.time() + 60
    while packer_stats(c)[2] < put_errors + 1:  # a copy attempted and failed
        assert time.time() < end, f'no failed copy: {packer_stats(c)}\n{logs(c)[-3000:]}'
        time.sleep(0.5)
    time.sleep(2)
    now = packer_stats(c)
    assert now[0] == base, f'packed on one fragment: {now} from {base}'
    assert len(degraded_writes(c, seen)) == 0, degraded_writes(c, seen)
    assert segcount(c, 1)[0] == 0, status(c, 1)
    for k in range(4):
        assert (a.path / f'one-{k}').read_bytes() == payload(f'one-{k}', files[f'one-{k}'])
    (flags / 'refuse-2').unlink()
    c.restart(3)
    up(c, 3)
    base = packed_total(c)
    wait_packed(c, base, 4, 'refused files once node 3 is back', timeout=120)
    print(f'5. node 3 down and node 2 refusing: {now[2] - put_errors} copies with one durable '
          f'fragment cancelled, nothing packed or marked, files exact; packed once node 3 returned PASS')

    # 6. Single-member outages after the rebuilds.
    wait_up_status(c)
    wait_segs(c, 1, lambda n, u: n == 0, 'before the outages')
    for down, via in ((1, 2), (2, 3), (3, 1)):
        a.close()
        mounts.remove(a)
        c.stop(down)
        verify(c, via, f'node {down} down')
        c.restart(down)
        up(c, down)
        wait_up_status(c)
        a = Mount(c, 1, f'mnt{down}')
        mounts.append(a)
    print('6. every single-member outage after the rebuilds: all files exact PASS')

    # 7. A second loss before the rebuild: every segment of this batch is
    # written without node 3, then loses its node 2 fragment too.
    c.stop(3)
    seen = degraded_writes(c)
    base = packed_total(c)
    batch(a, 'loss', 4)
    wait_packed(c, base, 4, 'loss')
    dw = degraded_writes(c, seen)
    assert dw, logs(c)[-3000:]
    for g, sid, gen, fi, node in dw:
        victims = [p for k, p in seg_files(c, 2).items() if k[1] == sid and k[2] == gen]
        assert len(victims) == 1, victims
        victims[0].unlink()
    c.restart(3)
    up(c, 3)
    wait_segs(c, 1, lambda n, u: u == len(dw), 'second loss')
    for g, sid, gen, fi, node in dw:
        end = time.time() + 30
        while count(c, rf'g{g} id={sid:016x} gen={gen:x} UNRECOVERABLE') == 0:
            assert time.time() < end, logs(c)[-4000:]
            time.sleep(0.5)
    time.sleep(5)
    st = status(c, 1)
    assert segcount(c, 1) == (len(dw), len(dw)), st
    assert 'Protection: LOST' in st and 'Cluster state: DEGRADED (3 up, 0 down)' in st, st
    for g, sid, gen, fi, node in dw:
        assert count(c, rf'rebuilt g{g} id={sid:016x} gen={gen:x}') == 0
    names = [f'loss-{k}' for k in range(4)]
    m = Mount(c, 1, 'lossmnt')
    try:
        for name in names:
            try:
                (m.path / name).read_bytes()
                raise AssertionError(f'{name}: read without two fragments')
            except OSError as e:
                assert e.errno == errno.EIO, e
    finally:
        m.close()
    lost.update(names)
    verify(c, 2, 'beside the unrecoverable segment')
    for name in names:
        os.unlink(a.path / name)
        files.pop(name)
    lost.clear()
    wait_segs(c, 1, lambda n, u: n == 0, 'unrecoverable segment removed', timeout=180)
    st = status(c, 1)
    assert 'Protection: full' in st, st
    print(f'7. second loss before the rebuild: {len(dw)} segments reported unrecoverable, never '
          f'healed, their files EIO, others exact; removing them retired the marks PASS')
    verify(c, 3, 'final')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_K37_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'K37 segment rebuild on return: audit on return, writes and compaction with a member down, '
      f'never on one fragment, second loss unrecoverable: PASS ({time.time() - t0:.1f} s, '
      f'{len(files)} files exact)')
