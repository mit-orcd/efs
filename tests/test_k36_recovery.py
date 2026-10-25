#!/usr/bin/env python3
"""K36 / F23 (D42): verified recovery after an unclean stop, on real daemons.

Private efsd daemons on loopback (tests/k33_daemons.py, tests/faults/efsd
with EFS_FAULTS hooks) and efs-fuse; stores under TMPDIR, free ports, never
17xxx. Large files only (never packed). Every crash is a SIGKILL or an
EFS_FAULT_CRASH / EFS_FAULT_COMPACT_DIE exit, so the store is left as a
killed process leaves it; acknowledged-but-unsynced loss is emulated by
truncating (torn) and deleting (missing) fragment files of the stopped
store, since a process kill keeps the page cache.

1. Three nodes form; files A0..A3 written and fsynced through mount A.
2. During a PUT: node 3 exits right after acknowledging its 5th PUT while
   mount A writes W1 (no fsync). While it is down, one of its fragments of
   the fsynced files is truncated and another deleted. Every fsynced file
   reads back exact through the two peers (W1 is read after recovery: a
   chunk acknowledged degraded with node 3's fragment counted can have one
   other holder, K34/F24).
3. During recovery: node 3 restarts and exits after its recovery wrote the
   first degraded mark; it restarts again and recovery restarts safely.
4. The fence holds until proven: with EFS_FAULT_RECOVERY_HOLD_FILE recovery
   parks just before clearing it; sync-status shows `recovery running
   phase=fence`, data-commit to node 3 fails and a fresh mount's fsync
   proves its stripe on the other two only (K35: marked degraded). Released:
   `recovery: verified`, data-commit succeeds and a fresh fsync proves all
   three fragments; the fragments node 3's crashed run acknowledged to
   mount A stay lost in its fsync (the ambiguous write is not proven). The interrupted run dropped the
   torn file; sync-status counts both losses as missing and marked; W154
   rebuilds every marked fragment (`rebuilt=2` or more, `rebuild_waiting=0`).
5. No acknowledged byte is lost while two peers hold it: node 1 stops and a
   fresh mount on node 2 reads every file exact, which needs node 3's
   rebuilt fragments.
6. During publication: node 2 exits applying its first publication entry
   (committed, not applied); after restart it verifies and every file is
   exact.
7. During reclaim: node 3 exits after journaling a reclaim batch (small
   files packed, files deleted); after restart the journal replays and
   recovery verifies.
8. During compaction: node 1, restarted with a 64 KiB memtable and 1 MiB L0
   cap, exits at its first KV compaction output (EFS_FAULT_COMPACT_DIE=1;
   usually at once, since the shrunken memtable forces a compaction of the
   existing KV, else under mkdir/rmdir churn); after restart it verifies.
9. Verification fails: a damaged storage session certificate on node 2
   leaves it `recovery FAILED` with the reason and fenced (data-commit
   fails); with the certificate gone, the next start verifies.
10. A group this node does not host: node 4 joins (node 1's group-2 replica
    retires); node 1 is killed and its recovery reads group 2's inventory
    from a hosting member (groups proven=1/2) and verifies.
11. Packed segments: small files are packed; a member holding segment
    fragments is killed and one of them deleted; its recovery reports the
    referenced segment fragment missing and marks it (K37), verifies, every
    file still reads exact, and the K37 rebuild clears the mark
    (tests/test_k36_segment_recovery.py covers this path in depth).
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
    print('K36 recovery: NOTRUN (no /dev/fuse, fusermount3 or efs-fuse)')
    sys.exit(0)
work = Path(tempfile.mkdtemp(prefix='efs-k36-', dir=os.environ.get('TMPDIR')))
env = {k: v for k, v in os.environ.items() if not k.startswith('EFS_FAULT_')}
env.update(EFS_PACK_MIN_AGE_MS='500', EFS_PACK_INTERVAL_MS='200')
SIZES = (1024 * 1024 + 3, 300000, 2 * 1024 * 1024 + 11, 70001)
files = {}
notes = []
t0 = time.time()


def put(m, name, size, sync=False):
    data = payload(name, size)
    with open(m.path / name, 'wb') as f:
        f.write(data)
        f.flush()
        if sync:
            os.fsync(f.fileno())
    files[name] = size


def fsync_rc(m, name):
    """0 when write + fsync of a new small file succeeds, else the errno."""
    try:
        with open(m.path / name, 'wb') as f:
            f.write(payload(name, 4096))
            f.flush()
            os.fsync(f.fileno())
        files[name] = 4096
        return 0
    except OSError as e:
        return e.errno


def verify(m, what, names=None):
    for name, size in sorted(files.items()):
        if names is not None and name not in names:
            continue
        got = (m.path / name).read_bytes()
        assert got == payload(name, size), f'{what}: {name} differs ({len(got)} of {size} B)'
    return len(files)


def frags(c, i):
    out = []
    for dirpath, _, names in os.walk(c.dir / f'n{i}' / 'data' / 'exports'):
        out += [Path(dirpath) / n for n in names if re.match(r'^\d+\.\d+\.\d+$', n)]
    return sorted(out)


def segs(c, i):
    out = []
    for dirpath, _, names in os.walk(c.dir / f'n{i}' / 'data' / 'segments'):
        out += [Path(dirpath) / n for n in names if re.match(r'^[0-9a-f]{16}\.[0-9a-f]+\.\d$', n)]
    return sorted(out)


def packed_total(c):
    """Files the packers switched into segments, summed over every daemon
    run and group (counters restart with a process or a term)."""
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


def status(c, i):
    rc, out = mgmt(c.env, 'sync-status', c.ep(i))
    m = re.search(r'^  recovery (\S+)(?: phase=(\S+))? (\d+) ms: (.*)$', out, re.M)
    if not m:
        return None, out
    st = {'state': m.group(1), 'phase': m.group(2)}
    for k, v in re.findall(r'(\w+)=(\d+)', m.group(4)):
        st.setdefault(k, int(v))
    g = re.search(r'segments checked=(\d+) missing=(\d+)', out)
    st['segments'] = (int(g.group(1)), int(g.group(2))) if g else None
    g = re.search(r'groups proven=(\d+)/(\d+)', out)
    st['groups'] = (int(g.group(1)), int(g.group(2))) if g else None
    r = re.search(r'^    reason: (.*)$', out, re.M)
    st['reason'] = r.group(1) if r else ''
    return st, out


def wait_status(c, i, want, timeout=90):
    end, last = time.time() + timeout, None
    while time.time() < end:
        try:
            st, out = status(c, i)
            last = out
            if st and want(st):
                return st
        except Exception as e:  # noqa: BLE001 - daemon still starting
            last = repr(e)
        time.sleep(0.3)
    raise AssertionError(f'node {i}: status never matched\n{last}\n{c.last_run(i)[-3000:]}')


def kill(c, i):
    c.stop(i, sig=signal.SIGKILL)


def up(c, i, n=3, timeout=90):
    c.wait_log(i, rf'raft-host: up raft_id={i - 1} n={n} formed', timeout=timeout)


def crashed(c, i, rc_want, timeout=120):
    end = time.time() + timeout
    while time.time() < end:
        p = c.procs.get(i)
        if p is not None and p.poll() is not None:
            rc = c.wait_exit(i)
            assert rc == rc_want, (i, rc, c.last_run(i)[-2000:])
            return
        time.sleep(0.1)
    gc = '\n'.join(f'node {k}: ' + ' | '.join(re.findall(r'(gc-health: .*|reclaim: .*|raft-host: gc.*)',
                                                            c.last_run(k))[-3:])
                   for k in sorted(c.procs))
    raise AssertionError(f'node {i} did not exit {rc_want}\n{c.last_run(i)[-2000:]}\n{gc}')


def verified(c, i, timeout=120):
    c.wait_log(i, r'recovery: verified in \d+ ms', timeout=timeout)
    return wait_status(c, i, lambda s: s['state'] == 'verified')


def rebuilt(c, i, timeout=180):
    """Every mark node i's recovery wrote or found is retired."""
    return wait_status(c, i, lambda s: s['state'] == 'verified' and s['rebuild_waiting'] == 0,
                       timeout=timeout)


def whole(c, timeout=240):
    """No degraded stripe left anywhere (W154 rebuilt every mark, including
    those of files written while a node was down): the next outage may take
    a fragment away without leaving a stripe with one."""
    end, out = time.time() + timeout, ''
    while time.time() < end:
        live = [i for i in sorted(c.procs) if c.alive(i)]
        rc, out = mgmt(c.env, 'status', c.ep(live[0]))
        if re.search(r'^Degraded chunks: 0 \(', out, re.M):
            return
        time.sleep(1)
    raise AssertionError(f'stripes still degraded\n{out}')


c = Cluster(work, 'k36', [1, 2, 3, 4], env)
mounts = []
try:
    # 1. Form, write and fsync.
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    a = Mount(c, 1, 'mnt-a')
    mounts.append(a)
    for k, size in enumerate(SIZES):
        put(a, f'A{k}', size, sync=True)
    before = frags(c, 3)
    assert len(before) >= 8, before
    for i in (1, 2, 3):
        st, out = status(c, i)
        assert st is None and 'recovery uncertain' not in out, out
    print(f'1. formed, {len(files)} files fsynced, node 3 holds {len(before)} fragments PASS',
          flush=True)

    # 2. Crash during a PUT, then lose two acknowledged fragments.
    c.stop(3)
    c.restart(3, extra_env={'EFS_FAULT_CRASH': 'put-acked:5'})
    up(c, 3)
    try:
        put(a, 'W1', 4 * 1024 * 1024 + 5)
    except OSError as e:
        notes.append(f'W1 write during the crash: {e}')
    crashed(c, 3, 137)
    assert 'fault: injected crash at put-acked' in c.last_run(3)
    torn, gone = before[0], before[-1]
    os.truncate(torn, 4096 + 100)
    gone.unlink()
    # W1 is checked after recovery: a chunk of it acknowledged degraded with
    # node 3's fragment counted may have only one other holder now (K34,
    # F24), which is not "two peers hold it".
    synced = [f'A{k}' for k in range(len(SIZES))]
    verify(a, 'node 3 down, two of its fragments lost', synced)
    print(f'2. node 3 killed after its 5th PUT acknowledgement; {torn.name} torn, '
          f'{gone.name} deleted; {len(synced)} fsynced files exact from two peers PASS',
          flush=True)

    # 3. Crash during recovery: restart, exit at the first mark, restart again.
    c.restart(3, extra_env={'EFS_FAULT_CRASH': 'recovery-marked'})
    crashed(c, 3, 137)
    run = c.last_run(3)
    assert 'recovery uncertain' in run and 'recovery: phase metadata' in run, run[-3000:]
    assert 'fault: injected crash at recovery-marked' in run, run[-3000:]
    print('3. node 3 killed during recovery after its first degraded mark PASS', flush=True)

    # 4. The fence holds until proven, then clears.
    hold = work / 'hold'
    hold.write_text('hold\n')
    c.restart(3, extra_env={'EFS_FAULT_RECOVERY_HOLD_FILE': str(hold)})
    up(c, 3)
    st = wait_status(c, 3, lambda s: s['phase'] == 'fence')
    assert st['state'] == 'running' and 'held by' in st['reason'], st
    assert st['missing'] >= 2 and st['marked'] + st['already_marked'] >= 2, st
    # The torn file was dropped by the interrupted run (step 3); recovery is
    # idempotent, so this run finds it simply missing.
    assert re.search(r'store: recovery dropped \S+/' + re.escape(torn.name), c.log(3)), torn
    rc, out = mgmt(c.env, 'data-commit', c.ep(3))
    assert rc != 0, ('data-commit while fenced', out)
    b = Mount(c, 2, 'mnt-b')
    mounts.append(b)
    # K35 proves fsync per stripe: while node 3 refuses COMMIT, a fresh write
    # is durable only on the other two, so its stripe is marked degraded
    # ("2 of 3 durable"), never counted with node 3's copy.
    assert fsync_rc(b, 'fresh-fenced') == 0, 'fresh fsync while node 3 is fenced'
    fenced_marks = b.logp.read_text().count('marked degraded for rebuild on return')
    assert fenced_marks >= 1, b.logp.read_text()[-2000:]
    hold.unlink()
    st = verified(c, 3)
    rc, out = mgmt(c.env, 'data-commit', c.ep(3))
    assert rc == 0, ('data-commit after recovery', out)
    assert fsync_rc(b, 'fresh-ok') == 0, 'fresh fsync after recovery'
    log_b = b.logp.read_text()
    assert log_b.count('marked degraded for rebuild on return') == fenced_marks, \
        ('after recovery node 3 COMMITs again: all three fragments durable', log_b[-2000:])
    # The earlier ambiguous write stays unproven: mount A's fragments that
    # node 3's crashed run acknowledged are lost (another incarnation), so
    # its fsync of W1 never counts them durable.
    old_a = a.logp.read_text().count('\n')
    if 'W1' in files:
        try:
            with open(a.path / 'W1', 'rb+') as f:
                os.fsync(f.fileno())
        except OSError as e:
            notes.append(f'W1 fsync after recovery: {e}')
        tail = ''.join(a.logp.read_text().splitlines(True)[old_a:])
        rows = re.findall(r'fsync ino=\d+ chunk=\d+ gen=\S+ nodes=(\d+),(\d+),(\d+) '
                          r'fragments=([A-Z])([A-Z])([A-Z])', tail)
        n3 = [r[3 + r[:3].index(str(c.node_id(3)))] for r in rows if str(c.node_id(3)) in r[:3]]
        assert n3 and 'D' not in n3, ('crashed-run fragments of node 3 counted durable', tail[-2000:])
        notes.append(f'W1 fsync after recovery: {len(n3)} stripes, node 3 fragment states '
                     f'{"".join(sorted(set(n3)))} (never D)')
    # Every mark this recovery wrote or found is retired by W154 (tickets the
    # client left ADMITTED may still settle later; nothing waits on them).
    st = wait_status(c, 3, lambda s: s['rebuilt'] >= 2 and s['rebuild_waiting'] == 0,
                     timeout=180)
    for f in (torn, gone):
        assert f.exists() and f.stat().st_size >= 4096 + 128, f
    notes.append('node 3 recovery: ' + ' '.join(f'{k}={st[k]}' for k in
                 ('checked', 'dropped', 'missing', 'marked', 'already_marked',
                  'rebuilt', 'superseded', 'unrecoverable', 'waiting')))
    print('4. fence held until verified (data-commit EIO, fresh fsync only 2 of 3 durable), '
          'then cleared (data-commit OK, fresh fsync 3 of 3); crashed-run fragments stay lost; '
          f'torn and missing fragments rebuilt ({notes[-1]}) PASS', flush=True)

    # 5. No acknowledged byte lost while two peers hold it.
    verify(b, 'all three up')
    whole(c)
    a.close()
    mounts.remove(a)
    c.stop(1)
    d = Mount(c, 2, 'mnt-d')
    mounts.append(d)
    verify(d, 'node 1 down: needs node 3 rebuilt fragments')
    d.close()
    mounts.remove(d)
    c.restart(1)
    up(c, 1)
    print(f'5. node 1 down: {len(files)} files exact from nodes 2 and 3 PASS', flush=True)

    # 6. Crash during publication.
    b.close()
    mounts.remove(b)
    c.stop(2)
    c.restart(2, extra_env={'EFS_FAULT_CRASH': 'publish-apply'})
    try:
        up(c, 2)
    except AssertionError:
        assert not c.alive(2), c.last_run(2)[-2000:]  # died replaying a publication
    a = Mount(c, 1, 'mnt-a2')
    mounts.append(a)
    for k in range(6):
        try:
            put(a, f'P{k}', SIZES[k % len(SIZES)])
        except OSError as e:
            notes.append(f'P{k} during node 2 crash: {e}')
    crashed(c, 2, 137)
    assert 'fault: injected crash at publish-apply' in c.last_run(2)
    c.restart(2)
    up(c, 2)
    verified(c, 2)
    rc, out = mgmt(c.env, 'data-commit', c.ep(2))
    assert rc == 0, out
    verify(a, 'after publication crash')
    whole(c)
    print('6. node 2 killed applying a publication; recovery verified, data-commit OK, '
          'files exact PASS', flush=True)

    # 7. Crash during reclaim.
    c.stop(3)
    c.restart(3, extra_env={'EFS_FAULT_CRASH': 'reclaim-accepted'})
    up(c, 3)
    # The packer hands the per-file fragments of packed small files to the
    # members' reclaim journals (GC_FRAG_BATCH); a reaped deletion does too
    # (GC_INODE_BATCH). Whichever batch reaches node 3 first.
    for k, size in enumerate((900, 4096, 12000, 16384, 2500, 7000)):
        put(a, f'r{k}', size)
    for name in ('A1', 'A3', 'P1'):
        if name in files:
            (a.path / name).unlink()
            del files[name]
    crashed(c, 3, 137, timeout=180)
    c.restart(3)
    up(c, 3)
    verified(c, 3)
    c.wait_log(3, r'reclaim: journal epoch=\d+ segments=\d+ records=\d+ replayed', timeout=5)
    verify(a, 'after reclaim crash')
    whole(c)
    print('7. node 3 killed after journaling a reclaim batch; journal replayed, recovery '
          'verified PASS', flush=True)

    # 8. Crash during KV compaction. The mount on node 2 comes first: a
    # mount started while node 1 is dying can time out. Node 1 comes back
    # before anything is read, so a stripe written while node 3 was down in
    # step 7 needs no third holder here.
    a.close()
    mounts.remove(a)
    b = Mount(c, 2, 'mnt-b2')
    mounts.append(b)
    c.stop(1)
    c.restart(1, extra_env={'EFS_FAULT_COMPACT_DIE': '1', 'EFS_FAULT_MEMTABLE_BYTES': '65536',
                            'EFS_FAULT_L0_BYTES': '1048576'})
    try:
        up(c, 1)
    except AssertionError:
        assert not c.alive(1), c.last_run(1)[-2000:]  # compacted (and died) while starting
    churn = b.path / 'churn'
    churn.mkdir()
    end = time.time() + 240
    k = 0
    while c.alive(1) and time.time() < end:
        for _ in range(50):
            try:
                (churn / f'd{k}').mkdir()
                (churn / f'd{k}').rmdir()
            except OSError:
                pass
            k += 1
    crashed(c, 1, 99, timeout=10)
    c.restart(1)
    up(c, 1)
    verified(c, 1)
    rc, out = mgmt(c.env, 'data-commit', c.ep(1))
    assert rc == 0, out
    verify(b, 'after compaction crash')
    whole(c)
    notes.append(f'compaction crash after {k} mkdir/rmdir pairs')
    print(f'8. node 1 killed at its first KV compaction output ({k} mkdir/rmdir); recovery '
          'verified PASS', flush=True)

    # 9. Verification itself fails: the store stays fenced with the reason.
    kill(c, 2)
    certs = c.dir / 'n2' / 'meta' / 'put-session'
    certs.mkdir(parents=True, exist_ok=True)
    bad = certs / ('ab' * 16)
    bad.write_bytes(b'not a certificate')
    c.restart(2)
    up(c, 2)
    c.wait_log(2, r'recovery: FAILED in phase ledgers: storage session certificate', timeout=60)
    st = wait_status(c, 2, lambda s: s['state'] == 'FAILED')
    assert 'storage session certificate' in st['reason'], st
    rc, out = mgmt(c.env, 'data-commit', c.ep(2))
    assert rc != 0, ('data-commit while recovery failed', out)
    c.stop(2)
    bad.unlink()
    c.restart(2)
    up(c, 2)
    verified(c, 2)
    whole(c)
    print('9. damaged certificate: recovery FAILED with the reason, store fenced; '
          'restart without it verified PASS', flush=True)

    # 10. A group this node does not host: its inventory from a hosting member.
    c.start(4, join=2)
    c.wait_log(4, r'raft-host: up raft_id=3 n=4 formed', timeout=90)
    c.wait_members([1, 2, 3, 4], 4)
    for k in range(2):
        put(b, f'G{k}', SIZES[k])
    end = time.time() + 90
    while time.time() < end and c.status(1).get(2, (1,))[0]:
        time.sleep(0.5)
    assert not c.status(1)[2][0], c.status(1)
    kill(c, 1)
    c.restart(1, quota='1G')
    up(c, 1, n=4)
    st = verified(c, 1)
    assert st['groups'] == (1, 2), st
    verify(b, 'after node 1 recovery at four members')
    print(f'10. node 1 hosts group 0 only: recovery read group 2 from a member, groups '
          f'proven={st["groups"][0]}/{st["groups"][1]}, verified PASS', flush=True)

    # 11. Packed segments: a referenced segment fragment lost.
    base = packed_total(c)
    for k, size in enumerate((1, 512, 4096, 9000, 16384, 777, 3000, 12000)):
        put(b, f's{k}', size)
    end = time.time() + 90
    while packed_total(c) < base + 8 and time.time() < end:
        time.sleep(0.3)
    assert packed_total(c) >= base + 8, (base, packed_total(c))
    # One loss only: a segment the packer wrote while a member was still
    # in its peer cooldown after case 10's restart is already marked
    # degraded (K37 `written degraded`), and deleting a second fragment of
    # it is a correct UNRECOVERABLE, not this case. Wait until the rebuild
    # on return has retired every segment mark before choosing the victim.
    end = time.time() + 120
    while True:
        rc, out = mgmt(c.env, 'status', c.ep(1))
        m = re.search(r'^Degraded segments: (\d+) ', out, re.M)
        if rc == 0 and m and int(m.group(1)) == 0:
            break
        assert time.time() < end, out
        time.sleep(0.5)
    holder = next((i for i in (2, 3, 4) if segs(c, i)), None)
    assert holder, 'no member holds a segment fragment'
    kill(c, holder)
    lost = segs(c, holder)[0]
    lost.unlink()
    c.restart(holder)
    up(c, holder, n=4)
    st = verified(c, holder)
    assert st['segments'][0] >= 1 and st['segments'][1] >= 1, st
    assert re.search(r'recovery: segment g\d+ id=\S+ gen=\S+ fragment \d not proven here',
                     c.last_run(holder)), c.last_run(holder)[-2000:]
    assert re.search(r'recovery: segment g\d+ id=\S+ gen=\S+ fragment \d (?:already )?marked '
                     r'degraded for rebuild', c.last_run(holder)), c.last_run(holder)[-2000:]
    verify(b, 'segment fragment lost on one member')
    st = wait_status(c, holder, lambda s: re.search(r'segments .*? rebuilt=[1-9]\d* waiting=0;',
                                                     status(c, holder)[1]) is not None,
                     timeout=180)
    print(f'11. node {holder}: a referenced segment fragment deleted; recovery found it '
          f'missing and marked it (segments checked={st["segments"][0]} '
          f'missing={st["segments"][1]}), verified; rebuilt by K37; files exact PASS', flush=True)
finally:
    for m in mounts:
        m.close()
    c.stop_all()
    if os.environ.get('EFS_K36_KEEP'):
        print('kept', work)
    else:
        shutil.rmtree(work, ignore_errors=True)
for n in notes:
    print(n)
print(f'K36 recovery: crash during PUT, publication, reclaim, compaction and recovery; torn and '
      f'missing fragments; fence held then cleared; COMMIT after recovery, crashed-run '
      f'fragments stay unproven; no acknowledged byte lost: PASS ({time.time() - t0:.1f} s)')
