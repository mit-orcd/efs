#!/usr/bin/env python3
"""B6 (alpha 1.0 checklist): one fragment destination made unavailable in a
controlled way, for ordinary chunks and packed files, during write, fsync
and read.

Real efsd daemons on loopback (tests/k33_daemons.py, the EFS_FAULTS build
tests/faults/efsd) and efs-fuse; private stores under TMPDIR, free ports
(never 17xxx), discovery off, GC on, the packer on (500 ms minimum age).
Three members, so every stripe and every packed segment has a fragment on
node 3, the member made unavailable. Ordinary files are 300000 B (three
128 KiB chunks), packed files 1 B to 16 KiB. Writes go through mount A
(node 1); every read check is a fresh mount (no client or kernel cache).

Modes (one cluster each; EFS_B6_MODES=down,refuse,slow,full picks some;
the test recipe runs one mode per command so the pool overlaps them):
- down: node 3 stopped in order (SIGTERM);
- refuse: node 3 up, every fragment PUT, GET, COMMIT, segment PUT/GET and
  rebuild answered with an error (EFS_FAULT_DEST_FILE, `refuse`);
- slow: node 3 answers those operations only after SLOW_MS (8 s), past the
  client's fragment deadlines (2 s per read fragment, 5 s per read pair,
  2 s PUT drain); during the fsync step its COMMIT is slowed past the
  client's 60 s COMMIT deadline (COMMIT_SLOW_MS); a second read step slows
  its fragment and segment GETs by READ_SLOW_MS (45 s), past any read
  deadline: every read must still reconstruct within READ_SLOW_BOUND_S;
- full: node 3 started with a 16 MiB quota and filled (B5's real quota
  refusal, no hook).
In each mode:
 1. Before: files written and closed with all up: ordinary R*, packed P*
    (waited until the packer moved them), and unsynced F* (closed, never
    fsynced).
 2. Fault on, while a 2 MiB file I0 is being written (down, refuse, slow):
    its write, close and fsync either fail or succeed with its stripes
    acknowledged without node 3 and marked (K34: no acknowledgement from a
    stopping member, K35: fsync only with proof).
 3. Read: every R* and P* through a fresh mount: exact bytes, or an
    explicit error; never wrong or short bytes. Within READ_BOUND_S each.
 4. fsync of every F*: success only with proof (two durable fragments plus
    a durable degraded mark, client log `marked degraded`), else EIO.
 5. Write: new ordinary W* and small Q* files, each written, closed and
    fsynced: success (a degraded acknowledgement with its W154 mark, never
    from node 3), or an explicit error (full: ENOSPC, no degraded mark).
    The degraded state shows: `Degraded chunks:` > 0 and `Protection:
    DEGRADED` (down, refuse); slow: that, or W154 already rebuilding the
    marks onto the slow member during the fault (status rebuilt=N rises:
    the member is up and its rebuild PUTs succeed after 8 s).
    K37: the packer may pack Q* during the fault only into a segment written
    on the other two fragments and marked (`written degraded` naming node
    3); those files read exact.
 6. Restore in order: fault off, node 3 stopped in order and started again
    (full: with a 1 GiB quota). W154 rebuilds every mark (`Degraded
    chunks: 0`, `Protection: full`), and K37's segment marks end (rebuilt
    or retired: `Degraded segments: 0`); every acknowledged file (R, P, F,
    W, Q) reads back exactly; the packer resumes and packs the Q* files
    written during the fault.
At the end every cluster is stopped in order and started again, and every
acknowledged file reads back exactly once more. Repair beyond W154 (a
member that never returns) is not claimed. EFS_B6_ARTIFACTS=DIR writes
b6-result.json (every bound and measured value) there.
Skipped (NOTRUN, exit 0) without /dev/fuse, fusermount3 or efs-fuse.
"""
import errno, json, os, re, shutil, sys, tempfile, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from k33_daemons import Cluster, EFSD, MGMT, Mount, fuse_ok, mgmt, payload  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not fuse_ok():
    print('B6 unavailable destination: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-b6-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items()
       if not k.startswith('EFS_FAULT_') and not k.startswith('EFS_TEST_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
env.pop('EFS_GC_DISABLE', None)
LARGE = 300000
INFLIGHT = 2 * 1024 * 1024 + 5   # 17 chunks, in flight when the fault starts
SMALL = (1, 512, 4096, 16384)
SLOW_MS = 8000
COMMIT_SLOW_MS = 65000
READ_SLOW_MS = 45000      # read step: past the whole read's deadline
READ_SLOW_BOUND_S = 12.0  # a read past that deadline: pair 5 s + parity 2 s + margin
READ_BOUND_S = 30.0       # one file read through a fresh mount
WRITE_BOUND_S = 90.0      # one file written, closed and fsynced
FSYNC_BOUND_S = 75.0      # one fsync (the client's COMMIT deadline is 60 s)
REBUILD_BOUND_S = 180.0   # W154 clears every mark after the restore
PACK_BOUND_S = 120.0      # the packer packs the files written during the fault
DATA_OPS = 'put,get,commit,segput,segget,rebuild'
modes = [m for m in (os.environ.get('EFS_B6_MODES') or 'down,refuse,slow,full').split(',') if m]
artifacts = Path(os.environ['EFS_B6_ARTIFACTS']) if os.environ.get('EFS_B6_ARTIFACTS') else None
result = {'bounds': {'read_s': READ_BOUND_S, 'write_s': WRITE_BOUND_S, 'fsync_s': FSYNC_BOUND_S,
                     'rebuild_s': REBUILD_BOUND_S, 'pack_s': PACK_BOUND_S,
                     'slow_ms': SLOW_MS, 'commit_slow_ms': COMMIT_SLOW_MS,
                     'read_slow_ms': READ_SLOW_MS, 'read_slow_s': READ_SLOW_BOUND_S},
          'modes': {}}
t0 = time.time()


def status(c, i=1):
    return mgmt(c.env, 'status', c.ep(i))[1]


def degraded(c, i=1):
    m = re.search(r'^Degraded chunks: (\d+) ', status(c, i), re.M)
    return int(m.group(1)) if m else None


def degraded_segments(c, i=1):
    m = re.search(r'^Degraded segments: (\d+) ', status(c, i), re.M)
    return int(m.group(1)) if m else None


def logs(c):
    return ''.join(c.log(i) for i in c.ports if (c.dir / f'n{i}.log').exists())


def degraded_writes(c):
    """(group, id, gen, fragment, node) of each K37 `written degraded` line."""
    return re.findall(r'packer: segment g(\d+) id=([0-9a-f]+) gen=([0-9a-f]+) written degraded: '
                      r'fragment (\d+) for node (\d+)', logs(c))


def segment_back(c, seg):
    """The missing fragment's file of a segment written degraded is on its node."""
    g, sid, gen, f, node = seg
    return (c.dir / f'n{node}' / 'data' / 'segments' / f'g{g}' / f'{int(sid, 16) & 0xfff:03x}' /
            f'{sid}.{gen}.{f}').exists()


def wait_degraded(c, pred, what, timeout):
    end, got = time.time() + timeout, None
    while time.time() < end:
        got = degraded(c)
        if got is not None and pred(got):
            return got
        time.sleep(0.5)
    raise AssertionError(f'{what}: degraded count {got}\n{status(c)}')


def rebuilt(c, i=1):
    """W154 rebuilds the group leaders report (`status` rebuilt=N)."""
    m = re.search(r'^Degraded chunks: .*?rebuilt=(\d+)', status(c, i), re.M)
    return int(m.group(1)) if m else 0


def packed_total(c):
    """Files the packers switched, summed over every daemon run and group."""
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


def wait_packed(c, want, what, timeout=PACK_BOUND_S):
    end = time.time() + timeout
    while packed_total(c) < want:
        assert time.time() < end, f'{what}: packed {packed_total(c)} of {want}'
        time.sleep(0.2)
    return packed_total(c)


def up(c, i, n=3):
    c.wait_log(i, rf'raft-host: up raft_id={i - 1} n={n} formed', timeout=60)


class Run:
    """One mode's cluster, its fault file and its acknowledged files."""

    def __init__(self, mode):
        self.mode = mode
        self.fault = work / f'{mode}-n3-fault'
        self.c = Cluster(work, mode, [1, 2, 3], env)
        self.acked = {}     # name -> size: write and close (and fsync) succeeded
        self.refused = {}   # name -> errno: an explicit refusal
        self.out = {'reads': {}, 'fsync': {}, 'writes': {}}
        self.mounts = []

    def start3(self, quota='1G'):
        return self.c.start(3, join=1, quota=quota,
                            extra_env={'EFS_FAULT_DEST_FILE': str(self.fault)})

    def mount(self, name, node=1):
        m = Mount(self.c, node, name)
        self.mounts.append(m)
        return m

    def unmount(self, m):
        m.close()
        self.mounts.remove(m)

    def client_log(self, m):
        return m.logp.read_text(errors='replace')

    def set_fault(self, line):
        tmp = self.fault.with_suffix('.tmp')
        tmp.write_text(line + '\n')
        tmp.rename(self.fault)

    def clear_fault(self):
        if self.fault.exists():
            self.fault.unlink()

    def write(self, m, name, size, sync=True):
        """Write, close and (sync) fsync one file; record the verdict."""
        t = time.time()
        try:
            fd = os.open(m.path / name, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            try:
                data = payload(name, size)
                off = 0
                while off < len(data):
                    off += os.write(fd, data[off:])
                if sync:
                    os.fsync(fd)
            finally:
                os.close(fd)
            self.acked[name] = size
            self.refused.pop(name, None)  # written again after the restore
            verdict = 'ok'
        except OSError as e:
            self.refused[name] = e.errno
            verdict = errno.errorcode.get(e.errno, str(e.errno))
        dt = time.time() - t
        assert dt <= WRITE_BOUND_S, f'{self.mode}: write {name} took {dt:.1f} s'
        return verdict, dt

    def read_check(self, m, names, what):
        """Exact bytes or an explicit error; never wrong or short bytes."""
        got = {}
        for name in names:
            t = time.time()
            try:
                data = (m.path / name).read_bytes()
                want = payload(name, self.acked[name])
                assert data == want, (f'{self.mode} {what}: {name} returned {len(data)} B, '
                                      f'not its {len(want)} acknowledged bytes')
                v = 'exact'
            except OSError as e:
                assert e.errno in (errno.EIO, errno.EBUSY, errno.ETIMEDOUT, errno.ENOSPC), e
                v = errno.errorcode.get(e.errno, str(e.errno))
            dt = time.time() - t
            assert dt <= READ_BOUND_S, f'{self.mode} {what}: read {name} took {dt:.1f} s'
            got[name] = (v, round(dt, 2))
        return got

    def verify_all(self, what):
        m = self.mount(f'v-{what}', node=2)
        try:
            got = self.read_check(m, sorted(self.acked), what)
        finally:
            self.unmount(m)
        bad = {k: v for k, v in got.items() if v[0] != 'exact'}
        assert not bad, f'{self.mode} {what}: acknowledged files not exact: {bad}'
        return max((v[1] for v in got.values()), default=0.0)

    def close(self):
        self.clear_fault()
        for m in list(self.mounts):
            m.close()
        self.mounts = []
        self.c.stop_all()


def fault_on(r):
    if r.mode == 'down':
        r.c.stop(3)
    elif r.mode == 'refuse':
        r.set_fault('refuse ' + DATA_OPS)
    elif r.mode == 'slow':
        r.set_fault(f'slow:{SLOW_MS} ' + DATA_OPS)
    elif r.mode == 'full':
        # Fill node 3 (16 MiB quota): every stripe has a fragment there, so
        # writes fill it until one is refused ENOSPC. Nothing degraded.
        a = r.a
        k = 0
        while True:
            v, _ = r.write(a, f'fill-{k}', 1 << 20)
            if v != 'ok':
                assert v == 'ENOSPC', v
                break
            k += 1
            assert k < 64, 'node 3 never filled'
        r.out['fill_files'] = k


def run_mode(mode):
    r = Run(mode)
    runs.append(r)
    c = r.c
    c.start(1)
    c.start(2, join=1)
    r.start3(quota='16M' if mode == 'full' else '1G')
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    r.a = a = r.mount('mnt-a')

    # 1. Before the fault.
    for k in range(3):
        assert r.write(a, f'R{k}', LARGE)[0] == 'ok'
    base = packed_total(c)
    for k, size in enumerate(SMALL):
        assert r.write(a, f'P{k}', size)[0] == 'ok'
    wait_packed(c, base + len(SMALL), 'before the fault')
    for k in range(3):
        assert r.write(a, f'F{k}', LARGE, sync=False)[0] == 'ok'
    for k, size in enumerate(SMALL):
        assert r.write(a, f'FS{k}', size, sync=False)[0] == 'ok'
    unsynced = [n for n in r.acked if n.startswith('F')]
    # 2. Fault on, while a multi-chunk write is in flight (not for full: the
    # fill is the fault). Its write, close and fsync must either fail or
    # succeed with every stripe acknowledged without node 3 marked.
    inflight = {}
    th = None
    if mode != 'full':
        th = threading.Thread(target=lambda: inflight.update(v=r.write(a, 'I0', INFLIGHT)), daemon=True)
        th.start()
        end = time.time() + 10
        while time.time() < end and (not (a.path / 'I0').exists() or
                                     (a.path / 'I0').stat().st_size < INFLIGHT // 4):
            time.sleep(0.02)
    fault_on(r)
    if th:
        th.join(timeout=WRITE_BOUND_S + 10)
        assert not th.is_alive(), f'{mode}: the in-flight write is still running'
        r.out['inflight_write'] = inflight['v']
    pk_before = packed_total(c)
    dw_before = set(degraded_writes(c))
    rebuilt_before = rebuilt(c)

    # 3. Read through a fresh mount.
    b = r.mount('mnt-b')
    reads = r.read_check(b, [n for n in r.acked if n[0] in 'RP'], 'read during the fault')
    r.unmount(b)
    r.out['reads'] = reads
    assert all(v[0] == 'exact' for v in reads.values()), \
        f'{mode}: a read did not reconstruct from the other two: {reads}'
    if mode == 'slow':
        # Past every read deadline: node 3's fragment and segment GETs
        # answer only after READ_SLOW_MS. A read waits a bounded time for
        # that member and reconstructs from the other two.
        r.set_fault(f'slow:{READ_SLOW_MS} get,segget')
        b = r.mount('mnt-b2')
        late = r.read_check(b, [n for n in r.acked if n[0] in 'RP'], 'read past the deadline')
        r.unmount(b)
        r.out['reads_past_deadline'] = late
        assert all(v[0] == 'exact' for v in late.values()), \
            f'{mode}: a read past the deadline did not reconstruct: {late}'
        assert max(v[1] for v in late.values()) <= READ_SLOW_BOUND_S, \
            f'{mode}: a read waited for the slow member: {late}'
        r.set_fault(f'slow:{SLOW_MS} ' + DATA_OPS)

    # 4. fsync of the unsynced files.
    if mode == 'slow':
        r.set_fault(f'slow:{COMMIT_SLOW_MS} commit')
    before = r.client_log(a).count('marked degraded')
    fs = {}
    for name in unsynced:
        t = time.time()
        fd = os.open(a.path / name, os.O_RDWR)
        try:
            os.fsync(fd)
            v = 'ok'
        except OSError as e:
            assert e.errno == errno.EIO, e
            v = 'EIO'
            r.acked.pop(name)   # not durable: never claimed
            r.refused[name] = e.errno
        finally:
            os.close(fd)
        dt = time.time() - t
        assert dt <= FSYNC_BOUND_S, f'{mode}: fsync {name} took {dt:.1f} s'
        fs[name] = (v, round(dt, 2))
        if mode == 'slow' and len(fs) == 1:
            # One fsync past the COMMIT deadline is the case; the rest
            # meet the ordinary slow member.
            r.set_fault(f'slow:{SLOW_MS} ' + DATA_OPS)
    marked = r.client_log(a).count('marked degraded') - before
    r.out['fsync'] = fs
    r.out['fsync_marked'] = marked
    if mode in ('down', 'refuse', 'slow'):
        # Every unsynced stripe had a fragment on node 3, which proves
        # nothing: success needs a durable mark per stripe first.
        assert all(v[0] == 'ok' for v in fs.values()), f'{mode}: fsync refused: {fs}'
        assert marked >= 1, r.client_log(a)[-3000:]
    else:
        assert all(v[0] == 'ok' for v in fs.values()), f'{mode}: fsync refused: {fs}'
        assert marked == 0, r.client_log(a)[-3000:]

    # 5. Write during the fault.
    if mode == 'slow':
        r.set_fault(f'slow:{SLOW_MS} ' + DATA_OPS)
    ws = {}
    for k in range(3):
        ws[f'W{k}'] = r.write(a, f'W{k}', LARGE)
    for k, size in enumerate(SMALL):
        ws[f'Q{k}'] = r.write(a, f'Q{k}', size)
    r.out['writes'] = {k: (v, round(dt, 2)) for k, (v, dt) in ws.items()}
    alog = r.client_log(a)
    from_3 = re.findall(r'degraded acknowledgement, fragment \d+ for node (\d+) not', alog)
    r.out['degraded_acks'] = len(from_3)
    if mode == 'full':
        assert all(v == 'ENOSPC' for v, _ in ws.values()), f'full: {ws}'
        assert not from_3, alog[-3000:]
        assert degraded(c) == 0, status(c)
        r.out['degraded_during'] = 0
    else:
        assert all(v == 'ok' for v, _ in ws.values()), f'{mode}: writes refused: {ws}'
        assert from_3 and set(from_3) == {'3'}, f'{mode}: degraded acks {from_3}\n' + alog[-3000:]
        if mode == 'slow':
            # A slow member is up and answers, late: the W154 pass may
            # rebuild its marks during the fault (rebuild PUTs wait 8 s and
            # succeed), so the count can be back at 0 before a sample. The
            # degraded state then shows as rebuilds the leaders report.
            end, n = time.time() + 60, 0
            while time.time() < end:
                n = degraded(c) or 0
                if n > 0 or rebuilt(c) > rebuilt_before:
                    break
                time.sleep(0.5)
            r.out['rebuilt_during_fault'] = rebuilt(c) - rebuilt_before
            assert n > 0 or r.out['rebuilt_during_fault'] > 0, \
                f'{mode}: degraded state never shown (count 0, no rebuild)\n{status(c)}'
            if n > 0:
                assert 'Protection: DEGRADED' in status(c) or degraded(c) == 0, status(c)
        else:
            n = wait_degraded(c, lambda n: n > 0, f'{mode}: during the fault', 60)
            st = status(c)
            assert 'Protection: DEGRADED' in st, st
        r.out['degraded_during'] = n
    # K37 policy (decided Oct 24): the packer may copy a segment while node 3
    # is unavailable only with the other two fragments acknowledged; the
    # segment is then marked degraded before any file is switched in, its
    # files read back from the two, and the mark ends on return (rebuilt, or
    # retired with the segment). slow: node 3 acknowledges late, so the
    # packer waits and packs on three, or marks if a PUT missed its deadline.
    r.out['packed_during_fault'] = packed_total(c) - pk_before
    dw = [s for s in degraded_writes(c) if s not in dw_before]
    r.out['segments_written_degraded'] = len(dw)
    assert all(node == '3' for *_, node in dw), f'{mode}: written degraded {dw}'
    if mode in ('down', 'refuse', 'full'):
        # Node 3 takes no segment fragment: every packed file went into a
        # segment written degraded and marked.
        assert r.out['packed_during_fault'] == 0 or dw, \
            f'{mode}: {r.out["packed_during_fault"]} files packed, no segment written degraded'
    r.out['degraded_segments_during'] = degraded_segments(c)
    if mode != 'full':
        b = r.mount('mnt-q')
        qr = r.read_check(b, [n for n in r.acked if n.startswith('Q')], 'packed during the fault')
        r.unmount(b)
        assert all(v[0] == 'exact' for v in qr.values()), f'{mode}: packed during the fault: {qr}'
        r.out['reads_packed_during_fault'] = qr

    # 6. Orderly restore and restart.
    r.clear_fault()
    if mode != 'down':
        c.stop(3)
    t = time.time()
    r.start3()
    up(c, 3)
    c.wait_members([1, 2, 3], 3)
    wait_degraded(c, lambda n: n == 0, f'{mode}: W154 after the restore', REBUILD_BOUND_S)
    # K37: segments copied during the fault are rebuilt and their marks clear.
    end = time.time() + REBUILD_BOUND_S
    while (degraded_segments(c) or 0) > 0 and time.time() < end:
        time.sleep(0.5)
    assert (degraded_segments(c) or 0) == 0, f'{mode}: K37 segments still degraded\n{status(c)}'
    # Recorded, not asserted: a mark also ends without a rebuild when its
    # segment is retired, e.g. compaction (which merges small segments, also
    # during the fault) moved its files on (PACK_RETIRE drops the mark).
    r.out['segments_fragment_back'] = sum(segment_back(c, s) for s in dw)
    assert 'Protection: full' in status(c), status(c)
    r.out['rebuild_s'] = round(time.time() - t, 1)
    r.out['rebuilt'] = sum(c.log(i).count('raft-host: rebuilt ino=') for i in (1, 2, 3))
    if mode == 'full':
        v, _ = r.write(a, 'after-full', LARGE)
        assert v == 'ok', v
        for k, size in enumerate(SMALL):
            assert r.write(a, f'Q{k}', size)[0] == 'ok'
    queued = [n for n in r.acked if n.startswith('Q')]
    t = time.time()
    wait_packed(c, pk_before + len(queued), f'{mode}: the packer after the restore')
    r.out['pack_after_restore_s'] = round(time.time() - t, 1)
    r.out['max_read_after_restore_s'] = r.verify_all('after-restore')
    print(f'B6 {mode}: reads {sorted(set(v[0] for v in reads.values()))}, fsync '
          f'{sorted(set(v[0] for v in fs.values()))} ({marked} marked), writes '
          f'{sorted(set(v for v, _ in ws.values()))}, degraded {r.out["degraded_during"]} '
          f'-> 0 in {r.out["rebuild_s"]} s, {len(r.acked)} acknowledged files exact PASS')
    result['modes'][mode] = r.out


runs = []
try:
    for mode in modes:
        run_mode(mode)
    # Every cluster stopped in order and started again: every acknowledged
    # file exact once more.
    for r in runs:
        for m in list(r.mounts):
            r.unmount(m)
        r.c.stop_all()
        r.c.start(1)
        r.c.start(2, join=1)
        r.start3()
        for i in (1, 2, 3):
            up(r.c, i)
        r.c.wait_members([1, 2, 3], 3)
        r.out['max_read_after_restart_s'] = r.verify_all('after-restart')
        r.out['acknowledged'] = len(r.acked)
        r.out['refused'] = {k: errno.errorcode.get(v, str(v)) for k, v in r.refused.items()}
        print(f'B6 {r.mode}: whole-cluster orderly restart, {len(r.acked)} acknowledged files '
              'exact PASS')
finally:
    for r in runs:
        r.close()
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
        # One file per mode selection: the recipe runs the four modes as
        # four commands and their results must not overwrite each other.
        tag = '' if modes == ['down', 'refuse', 'slow', 'full'] else '-' + '-'.join(modes)
        (artifacts / f'b6-result{tag}.json').write_text(json.dumps(result, indent=1, sort_keys=True))
    if os.environ.get('EFS_B6_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
print(f'B6 unavailable fragment destination ({",".join(modes)}): reads reconstruct, writes and '
      f'fsync only with proof, W154 clears after the restore, exact after restart: PASS '
      f'({time.time() - t0:.1f} s)')
