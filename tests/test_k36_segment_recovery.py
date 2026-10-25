#!/usr/bin/env python3
"""K36 + K37: a packed-segment fragment dropped by verified recovery is
marked before the fence clears and rebuilt.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build)
and efs-fuse; private stores under TMPDIR, free ports (never 17xxx). Three
members, one mount through node 1, small files (1 B to 16 KiB) packed by
the server packer (500 ms minimum age), compaction kept out of the window.
Fault hooks, each toggled per step so the run is deterministic:
  EFS_FAULT_CRASH=<point>          K36 crash points (_exit 137);
  EFS_FAULT_RECOVERY_HOLD_FILE=F   recovery parks before clearing its fence;
  EFS_FAULT_SEGMENT_REBUILD_HOLD=F K37 segment rebuilds wait while F exists.

1. Baseline: two batches packed with all members up, no segment mark.
2. Crash a member: node 3 exits right after acknowledging its first chunk
   PUT (EFS_FAULT_CRASH=put-acked). While it is down one of its segment
   fragments is deleted and another torn; every packed file reads back
   from the two peers.
3. Crash during recovery: node 3 restarts and exits right after its
   recovery committed the first segment mark (recovery-segment-marked).
4. The fence clears only once every dropped fragment is marked: restarted
   with the fence held, recovery reports both fragments missing and marked
   (one found already marked from the crashed run), `efs-mgmt status`
   counts both degraded segments while node 3 is still fenced (the group
   leader's audit does not answer before the fence clears, so these marks
   are recovery's), data-commit to node 3 fails.
5. Rebuild: the fence clears; with the rebuild released the group leaders
   restore both fragments (files present, full length, verified against
   the segment digests), the marks clear, node 3's recovery counts both
   `rebuilt`, and its audit marks nothing more (one mechanism).
6. Every packed file reads back exactly, then again through a single-member
   outage of each other member (node 1, then node 2), which needs node 3's
   rebuilt fragments.
7. A second loss before the rebuild: with the rebuild held, new files are
   packed; node 3 is killed, its fragments of the new segments deleted, and
   its recovery marks them; then node 2 is killed and its fragments of the
   same segments deleted. Node 2's recovery reports each segment
   UNRECOVERABLE, and once released the rebuild never heals them:
   `Protection: LOST`, their files read EIO, every other file reads back;
   removing their files retires the marks (`Protection: full`).
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import errno, os, re, shutil, signal, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('K36 segment recovery: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k36s-', dir=os.environ.get('TMPDIR')))
flags = work / 'flags'
flags.mkdir()
rb_hold = flags / 'rebuild-hold'
fence_hold = flags / 'fence-hold'
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200',
           EFS_PACK_COMPACT_IDLE_S='1000000',
           # the daily compaction window (2 h) starts 12 h from now
           EFS_PACK_COMPACT_DAILY_START_MIN=str((time.gmtime().tm_hour * 60 + time.gmtime().tm_min
                                                 + 720) % 1440),
           EFS_FAULT_SEGMENT_REBUILD_HOLD=str(rb_hold))
SIZES = (1, 300, 512, 4096, 5000, 9000, 12000, 16384)
files = {}
lost = set()
t0 = time.time()


def write(m, name, size):
    (m.path / name).write_bytes(payload(name, size))
    files[name] = size


def batch(m, tag, n=len(SIZES)):
    for k in range(n):
        write(m, f'{tag}-{k}', SIZES[k % len(SIZES)])


def status(c, i):
    return mgmt(c.env, 'status', c.ep(i))[1]


def live(c):
    return next(i for i in sorted(c.procs) if c.alive(i))


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


def whole(c, timeout=240):
    """No degraded chunk or segment left: the next outage may take a fragment."""
    end, out = time.time() + timeout, ''
    while time.time() < end:
        out = status(c, live(c))
        if re.search(r'^Degraded chunks: 0 \(', out, re.M) and \
                re.search(r'^Degraded segments: 0 \(', out, re.M):
            return
        time.sleep(1)
    raise AssertionError(f'still degraded\n{out}')


def logs(c, ids=(1, 2, 3)):
    return ''.join(c.log(i) for i in ids if (c.dir / f'n{i}.log').exists())


def count(c, pattern):
    return len(re.findall(pattern, logs(c)))


def packed_total(c):
    rc, out = mgmt(c.env, 'pack-compact', c.ep(live(c)), '0')
    got = re.findall(r'files_packed=(\d+)', out)
    assert got, out
    return sum(int(g) for g in got)


def wait_packed(c, base, n, what, timeout=120):
    end = time.time() + timeout
    while packed_total(c) < base + n:
        assert time.time() < end, f'{what}: packer moved {packed_total(c) - base} of {n}'
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


def recovery(c, i):
    """Node i's recovery line from sync-status: state, phase, reason and the
    segment counters."""
    rc, out = mgmt(c.env, 'sync-status', c.ep(i))
    m = re.search(r'^  recovery (\S+)(?: phase=(\S+))? \d+ ms: .*?; segments checked=(\d+) '
                  r'missing=(\d+) marked=(\d+) already_marked=(\d+) superseded=(\d+) '
                  r'unrecoverable=(\d+) rebuilt=(\d+) waiting=(\d+);', out, re.M)
    if not m:
        return None
    keys = ('checked', 'missing', 'marked', 'already_marked', 'superseded', 'unrecoverable',
            'rebuilt', 'waiting')
    st = dict(zip(keys, map(int, m.groups()[2:])))
    st.update(state=m.group(1), phase=m.group(2))
    r = re.search(r'^    reason: (.*)$', out, re.M)
    st['reason'] = r.group(1) if r else ''
    return st


def wait_recovery(c, i, want, what, timeout=120):
    end, st = time.time() + timeout, None
    while time.time() < end:
        try:
            st = recovery(c, i)
            if st and want(st):
                return st
        except Exception as e:  # noqa: BLE001 - daemon still starting
            st = repr(e)
        time.sleep(0.3)
    raise AssertionError(f'node {i} {what}: {st}\n{c.last_run(i)[-3000:]}')


def crashed(c, i, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        p = c.procs.get(i)
        if p is not None and p.poll() is not None:
            rc = c.wait_exit(i)
            assert rc == 137, (i, rc, c.last_run(i)[-2000:])
            return
        time.sleep(0.1)
    raise AssertionError(f'node {i} did not crash\n{c.last_run(i)[-2000:]}')


def up(c, i):
    c.wait_log(i, r'raft-host: up raft_id=\d+ n=3 formed', timeout=90)


def verify(c, node, what):
    m = Mount(c, node, f'v{int(time.time() * 1000)}')
    try:
        for name, size in files.items():
            if name in lost:
                continue
            got = (m.path / name).read_bytes()
            assert got == payload(name, size), f'{what}: {name} differs ({len(got)} of {size} B)'
    finally:
        m.close()


def seg_pat(key):
    g, sid, gen, fi = key
    return rf'g{g} id={sid:016x} gen={gen:x} fragment {fi}'


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
    batch(a, 'p0')
    wait_packed(c, base, len(SIZES), 'batch 0')
    batch(a, 'p1')
    wait_packed(c, base, 2 * len(SIZES), 'batch 1')
    wait_segs(c, 1, lambda n, u: n == 0, 'baseline')
    n3 = seg_files(c, 3)
    assert len(n3) >= 2, n3
    id3 = c.node_id(3)
    print(f'1. baseline: {packed_total(c) - base} files packed, node 3 (id {id3}) holds '
          f'{len(n3)} segment fragments, no mark PASS', flush=True)

    # 2. Crash a member, then damage two of its segment fragments.
    c.stop(3)
    c.restart(3, extra_env={'EFS_FAULT_CRASH': 'put-acked'})
    up(c, 3)
    big = 'big'
    try:
        write(a, big, 2 * 1024 * 1024 + 11)
    except OSError as e:
        files.pop(big, None)
        print(f'   note: {big} during the crash: {e}', flush=True)
    crashed(c, 3)
    assert 'fault: injected crash at put-acked' in c.last_run(3)
    (gone, gone_p), (torn, torn_p) = sorted(n3.items())[:2]
    gone_p.unlink()
    with open(torn_p, 'r+b') as f:
        f.truncate(4096)
    for name, size in files.items():
        if name != big:
            assert (a.path / name).read_bytes() == payload(name, size), name
    print(f'2. node 3 killed after a PUT acknowledgement; segment fragment {gone_p.name} deleted, '
          f'{torn_p.name} torn; packed files exact from two peers PASS', flush=True)

    # 3. Crash during recovery, right after its first segment mark.
    rb_hold.touch()
    c.restart(3, extra_env={'EFS_FAULT_CRASH': 'recovery-segment-marked'})
    crashed(c, 3)
    run = c.last_run(3)
    assert 'fault: injected crash at recovery-segment-marked' in run, run[-3000:]
    assert len(re.findall(r'recovery: segment \S+ id=\S+ gen=\S+ fragment \d not proven here; '
                          r'dropped', run)) == 1, run[-3000:]
    assert re.search(r'store: recovery dropped \S+/' + re.escape(torn_p.name), run), run[-3000:]
    print('3. node 3 killed during recovery right after its first segment mark; the torn '
          'fragment dropped PASS', flush=True)

    # 4. The fence holds until both dropped fragments are marked.
    fence_hold.write_text('hold\n')
    c.restart(3, extra_env={'EFS_FAULT_RECOVERY_HOLD_FILE': str(fence_hold)})
    up(c, 3)
    st = wait_recovery(c, 3, lambda s: s['phase'] == 'fence', 'at the fence')
    assert st['state'] == 'running' and 'held by' in st['reason'], st
    assert st['missing'] == 2 and st['marked'] == 1 and st['already_marked'] == 1, st
    assert st['unrecoverable'] == 0 and st['waiting'] == 2, st
    run = c.last_run(3)
    for key in (gone, torn):
        assert re.search(r'recovery: segment ' + seg_pat(key) + r' (?:already )?marked degraded '
                         r'for rebuild', run), (key, run[-3000:])
    got = wait_segs(c, 1, lambda n, u: n == 2 and u == 0, 'marks while node 3 is fenced')
    assert count(c, rf'audit node {id3} group \d+: g\d+ \S+ \S+ fragment \d+ (?:missing|torn or '
                    rf'bad); marked degraded') == 0, 'audit marked before the fence cleared'
    rc, out = mgmt(c.env, 'data-commit', c.ep(3))
    assert rc != 0, ('data-commit while fenced', out)
    print(f'4. fence held: recovery dropped and marked both fragments (marked=1 already_marked=1), '
          f'status counts {got[0]} degraded segments while node 3 is fenced, data-commit refused '
          'PASS', flush=True)

    # 5. The fence clears; the rebuild restores both fragments and clears the marks.
    fence_hold.unlink()
    c.wait_log(3, r'recovery: verified in \d+ ms', timeout=60)
    rc, out = mgmt(c.env, 'data-commit', c.ep(3))
    assert rc == 0, ('data-commit after recovery', out)
    assert segcount(c, 1)[0] == 2, status(c, 1)
    rb_hold.unlink()
    wait_segs(c, 1, lambda n, u: n == 0, 'rebuild of the recovered fragments')
    for key, p in ((gone, gone_p), (torn, torn_p)):
        assert p.exists() and p.stat().st_size > 4096, p
        assert count(c, rf'segment-rebuild: rebuilt {seg_pat(key)} on node {id3} ') == 1, key
    st = wait_recovery(c, 3, lambda s: s['rebuilt'] == 2 and s['waiting'] == 0, 'rebuilt')
    assert count(c, rf'audit node {id3} group \d+: g\d+ \S+ \S+ fragment \d+ (?:missing|torn or '
                    rf'bad); marked degraded') == 0, 'the audit disagreed with recovery'
    print(f'5. fence cleared, both fragments rebuilt on node 3 and verified, marks cleared '
          f'(recovery: {" ".join(f"{k}={st[k]}" for k in ("checked", "missing", "marked", "already_marked", "rebuilt", "waiting"))}); '
          'the audit marked nothing PASS', flush=True)

    # 6. Every file exact, then through an outage of each other member.
    whole(c)
    verify(c, 2, 'all up')
    for down, via in ((1, 2), (2, 3)):
        a.close()
        mounts.remove(a)
        c.stop(down)
        verify(c, via, f'node {down} down: needs node 3 rebuilt fragments')
        c.restart(down)
        up(c, down)
        c.wait_members([1, 2, 3], 3)
        whole(c)
        a = Mount(c, 1, f'mnt{down}')
        mounts.append(a)
    print(f'6. {len(files)} files exact; node 1 down and node 2 down in turn: all exact PASS',
          flush=True)

    # 7. A second loss of the same segments before the rebuild.
    rb_hold.touch()
    before = set(seg_files(c, 3))
    base = packed_total(c)
    batch(a, 'loss', 4)
    wait_packed(c, base, 4, 'loss batch')
    new = sorted(set(seg_files(c, 3)) - before)
    assert new, 'no new segment'
    c.stop(3, sig=signal.SIGKILL)
    for key in new:
        seg_files(c, 3)[key].unlink()
    c.restart(3)
    up(c, 3)
    st = wait_recovery(c, 3, lambda s: s['state'] == 'verified', 'after the first loss')
    assert st['marked'] == len(new) and st['unrecoverable'] == 0, st
    c.stop(2, sig=signal.SIGKILL)
    victims = [k for k in seg_files(c, 2) if any(k[:3] == n[:3] for n in new)]
    assert len(victims) == len(new), (victims, new)
    for key in victims:
        seg_files(c, 2)[key].unlink()
    c.restart(2)
    up(c, 2)
    st = wait_recovery(c, 2, lambda s: s['state'] == 'verified', 'after the second loss')
    assert st['unrecoverable'] == len(new) and st['marked'] == len(new), st
    for key in victims:
        assert re.search(r'recovery: segment ' + seg_pat(key) + r' lost here while another '
                         r'fragment is already marked missing \(mask=\S+\): UNRECOVERABLE',
                         c.last_run(2)), (key, c.last_run(2)[-3000:])
    rb_hold.unlink()
    wait_segs(c, 1, lambda n, u: u == len(new), 'second loss')
    for g, sid, gen, fi in new:
        end = time.time() + 60
        while count(c, rf'g{g} id={sid:016x} gen={gen:x} UNRECOVERABLE') == 0:
            assert time.time() < end, logs(c)[-4000:]
            time.sleep(0.5)
    time.sleep(5)
    st = status(c, 1)
    assert segcount(c, 1)[1] == len(new) and 'Protection: LOST' in st, st
    for g, sid, gen, fi in new:
        assert count(c, rf'rebuilt g{g} id={sid:016x} gen={gen:x}') == 0, 'healed silently'
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
    verify(c, 3, 'beside the unrecoverable segments')
    for name in names:
        os.unlink(a.path / name)
        files.pop(name)
    lost.clear()
    wait_segs(c, 1, lambda n, u: n == 0, 'unrecoverable segments removed', timeout=180)
    assert 'Protection: full' in status(c, 1), status(c, 1)
    print(f'7. second loss before the rebuild: {len(new)} segments marked by node 3, then '
          f'reported UNRECOVERABLE by node 2 recovery, never healed, files EIO, others exact; '
          'removing them retired the marks PASS', flush=True)
    verify(c, 2, 'final')
finally:
    for mt in mounts:
        mt.close()
    c.stop_all()
    if os.environ.get('EFS_K36S_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'K36 segment recovery: dropped segment fragments marked before the fence, rebuilt by '
      f'K37, second loss unrecoverable: PASS ({time.time() - t0:.1f} s, {len(files)} files exact)')
