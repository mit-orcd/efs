#!/usr/bin/env python3
"""K38 on real daemons: a truncate sweep must not leave a stale CAS hint that
stalls the next publication of the swept chunk (`pub-stale why=image-base`
until `publish-stalled` and EIO; fixed by cbase_reconcile, 619eceb30).

Three private efsd of the EFS_FAULTS build, mounts A (node 1) and B (node 2).
For each of ROUNDS fresh files:
1. A writes three full chunks and fsyncs; the REPORT notes the K88 CAS hint
   (the last noted chunk, 2: cbase_slot drops the chunk index, so an inode
   has one hint slot and any sibling's commit replaces it).
2. B truncates to one chunk and fsyncs (A's view stays stale; the FileID is
   kept). Wait until the truncate sweep removed the rows: `gc-status`
   trunc_pending seen above 0 and back to 0 on every node that saw it (a
   sweep waits TRUNC_SWEEP_MIN_AGE_NS, 2 s).
3. A writes chunk 1 whole plus PART bytes of chunk 2 in one request, so both
   are one W77 unit (separate writes let chunk 1 commit alone and replace
   chunk 2's hint), and fsyncs. A full chunk is an unconditional image that
   never consults the hint; the partial chunk 2 finds its row gone (base 0).
Round 1 loses `why=authority` on chunk 1; the repull finds chunk 2 absent and
replays it without a captured view; the rebuild's cbase_apply turns base 0
into the swept hint, and the host answers `why=image-base` (the trigger).
Fixed: cbase_reconcile retires the hint, the next round publishes. Before the
fix every round repeats the record: `publish-stalled` and EIO at fsync.

Pass: every round shows the trigger (else "trigger not met": proves
nothing), its fsync succeeds, no `publish-stalled`, the file reads back
exact. One summary line per round for evidence loops. Skipped (NOTRUN)
without FUSE.
"""
import os, re, sys, tempfile, time
from pathlib import Path
from k33_daemons import SLOW, Cluster, Mount, fuse_ok, mgmt, payload

if not fuse_ok():
    print('K38 swept hint: NOTRUN (FUSE unavailable)')
    raise SystemExit(0)
CS = 128 * 1024          # EFS_DEFAULT_CHUNK_SIZE
PART = 4096              # a partial write: a full chunk is an unconditional image
ROUNDS = int(os.environ.get('K38_SWEPT_ROUNDS', '2'))
work = Path(tempfile.mkdtemp(prefix='efs-k38s-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_FAULT_', 'EFS_TEST_'))}
env.pop('EFS_GC_DISABLE', None)
c = Cluster(work, 'k38s', [1, 2, 3], env)
t0 = time.time()


def trunc_pending():
    out = {}
    for i in (1, 2, 3):
        rc, text = mgmt(env, 'gc-status', c.ep(i))
        m = re.search(r'trunc_pending group0=(\d+) group2=(\d+)', text)
        assert rc == 0 and m, text
        out[i] = int(m[1]) + int(m[2])
    return out


def wait_swept():
    """The sweep removed the truncated chunks' rows: the pending key a GC pass
    saw is gone again on every node that saw it."""
    seen, end = set(), time.time() + 60 * SLOW
    while time.time() < end:
        p = trunc_pending()
        seen |= {i for i, n in p.items() if n}
        if seen and not any(p[i] for i in seen):
            return
        time.sleep(0.1)
    raise AssertionError(f'truncate sweep not observed: seen={sorted(seen)} pending={p}')


def node_logs():
    return ''.join(c.log(i) for i in (1, 2, 3))


try:
    c.start(1); c.start(2, join=1); c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_members([1, 2, 3], 3)
    m = Mount(c, 1, 'mnt')
    b = Mount(c, 2, 'mntb')
    fails = []
    for r in range(ROUNDS):
        name = f'f{r}'
        first, whole, second = payload(name, 3 * CS), payload(name + '-1', CS), payload(name + '-2', PART)
        fd = os.open(m.path / name, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o644)
        ino = os.fstat(fd).st_ino
        outcome, fsync_s = 'not reached', 0.0
        try:
            assert os.pwrite(fd, first, 0) == 3 * CS
            os.fsync(fd)
            fb = os.open(b.path / name, os.O_RDWR)
            os.ftruncate(fb, CS)
            os.fsync(fb)
            os.close(fb)
            wait_swept()
            # One write request: chunks 1 and 2 are one W77 unit, so chunk 1
            # cannot commit alone and overwrite chunk 2's hint (one slot per inode).
            assert os.pwrite(fd, whole + second, CS) == CS + PART
            t = time.monotonic()
            try:
                os.fsync(fd)
                outcome = 'ok'
            except OSError as e:
                outcome = f'errno={e.errno}'
            fsync_s = time.monotonic() - t
        finally:
            try:
                os.close(fd)   # a stalled inode's writeback error also reaches close
            except OSError as e:
                outcome = f'close-errno={e.errno}' if outcome == 'ok' else outcome
        image_base = len(re.findall(rf'pub-stale why=image-base ino={ino} ci=2 ', node_logs()))
        stalled = len(re.findall(rf'publish-stalled ino={ino} ', m.text()))
        data = (b.path / name).read_bytes() if outcome == 'ok' else b''
        end = 2 * CS + PART   # nothing of the truncated first image may come back
        exact = data[:end] == first[:CS] + whole + second and not any(data[end:])
        print(f'K38 swept hint round={r} ino={ino} image_base={image_base} publish_stalled={stalled} '
              f'fsync={outcome} fsync_s={fsync_s:.2f} exact={int(exact)}', flush=True)
        if not image_base:
            fails.append(f'round {r}: trigger not met (no why=image-base for ino {ino} ci=2)')
        elif outcome != 'ok' or stalled or not exact:
            fails.append(f'round {r}: image-base={image_base} publish-stalled={stalled} fsync {outcome} '
                         f'exact={exact}')
    assert not fails, '\n'.join(fails) + '\n' + m.text()[-3000:]
    print(f'K38 swept CAS hint on real daemons: {ROUNDS} rounds triggered and published: PASS '
          f'({time.time() - t0:.1f} s)')
finally:
    c.stop_all()
    if os.environ.get('K38_SWEPT_KEEP') or sys.exc_info()[0] is not None:
        print('kept', work)
    else:
        import shutil
        shutil.rmtree(work, ignore_errors=True)
