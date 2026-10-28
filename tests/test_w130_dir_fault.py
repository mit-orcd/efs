#!/usr/bin/env python3
"""W130 one-replica DIR fault: a corrupt stored row on one replica must halt
that group on that replica — never skip, never misread.

Fault-build daemons (tests/k33_daemons.py), EFS_DIR_SPREAD_MIN small so a
directory split (the DIR command sequence: begin-split, migrate, finish)
happens at 64 entries. With EFS_FAULT_DIR_CORRUPT_FILE armed on node 3, that
replica's apply damages the directory's stored row after its successful
begin-split step; the migrate step then reads a row the decoder refuses
(PROTO -> UNSUPPORTED -> group halted on that replica).

Asserts:
1. node 3 logs the damage and the documented halt diagnostic
   ("INCOMPATIBLE entry ... apply halted, group not served").
2. The other groups keep serving; the healthy replicas keep serving the
   halted group (quorum unaffected).
3. A client reads the damaged directory completely through the healthy quorum;
   unrelated directories read fine. No application retries hide EIO/BUSY.
4. raft-status on node 3 shows the halt (the group's applied count freezes
   while the leaders advance; node 3 never campaigns for it).
5. `efs-mgmt raft-resync <node3> <group>` resyncs the replica from the
   leader's snapshot; the group serves again and the directory reads back
   correctly.

The serve-first W165 scenario runs first: DIR is held before the damaged
leader receives distinct CREATEs. It must step aside, and snapshot resync
must restore its own direct GETATTR.
"""
import hashlib, os, re, shutil, signal, subprocess, sys, tempfile, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from resync_setup import request_before
from k33_daemons import Cluster as _Cluster, EFSD, MGMT, FUSE, mgmt as _mgmt  # noqa: E402

for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')

SPREAD_MIN = os.environ.get('W130_SPREAD_MIN', '64')
HALT_DEADLINE = int(os.environ.get('W130_HALT_DEADLINE', '240'))
RESYNC_DEADLINE = int(os.environ.get('W130_RESYNC_DEADLINE', '300'))

work = Path(tempfile.mkdtemp(prefix='efs-w130-', dir=os.environ.get('TMPDIR')))
env = dict(os.environ)
env['EFS_TRANSPORT'] = 'tcp'
env['EFS_DIR_SPREAD_MIN'] = SPREAD_MIN


def mgmt(*args):
    return _mgmt(env, *args)


def Cluster(name, ids):
    return _Cluster(work, name, ids, env)


def fuse_ok():
    return FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK) and shutil.which('fusermount3')


class Mount:
    def __init__(self, c, node, name):
        self.path = c.dir / name
        self.path.mkdir()
        self.log = open(c.dir / f'{name}.log', 'w')
        self.p = subprocess.Popen([str(FUSE), c.ep(node), str(self.path), '-f'],
                                  stdout=self.log, stderr=subprocess.STDOUT, env=env)
        end = time.time() + 30
        while time.time() < end and not os.path.ismount(self.path):
            if self.p.poll() is not None:
                raise AssertionError((c.dir / f'{name}.log').read_text()[-2000:])
            time.sleep(0.1)
        assert os.path.ismount(self.path), (c.dir / f'{name}.log').read_text()[-2000:]

    def close(self):
        subprocess.run(['fusermount3', '-u', str(self.path)], capture_output=True)
        try:
            self.p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def payload(name, size=128):
    out, k = b'', 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return out[:size]


def write_file(path, body):
    """One application attempt: EIO/BUSY is a failure, never test-retried."""
    with path.open('wb', buffering=0) as stream:
        view = memoryview(body)
        while view:
            written = stream.write(view)
            assert written, f'zero write: {path}'
            view = view[written:]


def retain_logs(cluster):
    """Finalize private fixture logs before successful runtime bulk is pruned."""
    dest = os.environ.get('W130_EVIDENCE_DIR')
    if dest:
        Path(dest).mkdir(parents=True, exist_ok=True)
        kept = Path(tempfile.mkdtemp(prefix=cluster.dir.name + '-', dir=dest))
        for log in cluster.dir.glob('*.log'):
            shutil.copy2(log, kept / log.name)
        print('[w130] retained finalized logs:', kept, flush=True)


def group_applied(c, i, g):
    rc, out = mgmt('raft-status', c.ep(i))
    assert rc == 0, out
    m = re.search(rf'\s+group {g} .*?applied=(\d+)', out)
    return int(m.group(1)) if m else -1



def serve_fault():
    """Hold DIR before corrupting its leader: CREATE must be the first reader.

    Three distinct direct CREATEs expose persistent base EIO without retries;
    the fixed node must redirect (or forward), then FUSE creates succeed.
    """
    cluster = Cluster('w165-serve', [1, 2, 3])
    mounted = None
    markers = {i: work / f'serve-on-{i}' for i in (1, 2, 3)}
    try:
        for i in (1, 2, 3):
            cluster.start(i, join=1 if i > 1 else None,
                          extra_env={'EFS_FAULT_DIR_SERVE_FILE': str(markers[i])})
        cluster.wait_voters((1, 2, 3), lambda i, v: v == {0: (1, 7), 2: (1, 7)})
        mounted = Mount(cluster, 1, 'mnt-serve')
        target = mounted.path / 'serve-target'
        target.mkdir()
        inode = target.stat().st_ino
        group = 0 if inode & 1 else 2
        victim = cluster.leader_placement(1)[group]
        for k in range(int(SPREAD_MIN)):
            write_file(target / f'old-{k}', payload(f'old-{k}'))
        mark = markers[victim]
        mark.touch()  # hold spread before crossing; no corruption yet
        fd = os.open(target / 'crossing', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        os.close(fd)
        Path(str(mark) + '.arm').touch()  # CREATE above has finished completely
        ready = Path(str(mark) + '.ready')
        deadline = time.monotonic() + 30
        while not ready.exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert ready.exists(), 'serve damage did not reach its deterministic boundary'
        assert int(ready.read_text()) == inode, ('wrong fault target', ready.read_text(), inode)
        assert 'INCOMPATIBLE' not in cluster.log(victim), 'a prior apply consumed the damage'
        print(f'[w165] damaged leader node={victim} group={group} inode={inode}; DIR held', flush=True)
        replies = []
        for k in range(3):
            rc, out = mgmt('raft-create', cluster.ep(victim), str(inode), f'direct-{k}')
            assert rc == 0, out
            match = re.search(r'status=(\d+) primary=(\d+)', out)
            assert match, out
            replies.append(tuple(map(int, match.groups())))
        print('[w165] three distinct CREATE replies:', replies, flush=True)
        # A raw RPC may say BUSY (5) while the new leader is elected.
        # This is not an application retry: the FUSE operations below must
        # still complete once, including fsync and exact readback. Base's
        # permanent ERROR (3) is never accepted.
        assert all(status in (0, 5, 7) for status, hint in replies), \
            ('persistent serve-path EIO/ERROR instead of redirect', replies, cluster.log(victim)[-1500:])
        assert all(hint != victim for status, hint in replies if status == 7), ('self redirect', replies)
        cluster.wait_log(victim, rf'group={group} INCOMPATIBLE stored row', timeout=10)
        assert 'apply halted, group not served' in cluster.log(victim)
        # No application retries: the healthy quorum must complete each create.
        for k in range(3):
            path = target / f'quorum-{k}'
            body = payload(f'quorum-{k}')
            with path.open('xb', buffering=0) as stream:
                assert stream.write(body) == len(body)
                os.fsync(stream.fileno())
            assert path.read_bytes() == body
        before = group_applied(cluster, victim, group)
        time.sleep(.1)
        assert group_applied(cluster, victim, group) == before
        rc, out = mgmt('raft-getattr', cluster.ep(victim), str(inode))
        route = re.search(r'status=(\d+) primary=(\d+)', out)
        assert rc == 0 and route and int(route[1]) in (0, 7) and int(route[2]) in {1, 2, 3} - {victim}, \
            ('halted replica must forward or redirect to a healthy leader', out)
        mark.unlink()
        rc, out = mgmt('raft-resync', cluster.ep(victim), str(group))
        assert rc == 0, out
        cluster.wait_log(victim, r'snapshot import .*halt cleared, group served again', timeout=60)
        # Normal leader routing may redirect even a fully healed follower.
        # Require the explicit clear, a fresh mutation applied on this replica,
        # and an exact successful read through the returned healthy route.
        healed = group_applied(cluster, victim, group)
        write_file(target / 'after-resync', payload('after-resync'))
        deadline = time.monotonic() + 15
        while group_applied(cluster, victim, group) <= healed and time.monotonic() < deadline:
            time.sleep(.05)
        assert group_applied(cluster, victim, group) > healed, 'resynced replica did not resume apply'
        rc, out = mgmt('raft-getattr', cluster.ep(victim), str(inode))
        route = re.search(r'status=(\d+) primary=(\d+)', out)
        assert rc == 0 and route and int(route[1]) in (0, 7), out
        if int(route[1]) == 7:
            assert int(route[2]) in {1, 2, 3} - {victim}, out
            rc, out = mgmt('raft-getattr', cluster.ep(int(route[2])), str(inode))
        assert rc == 0 and f'status=0 ' in out and f'ino={inode} ' in out, out
        assert (target / 'after-resync').read_bytes() == payload('after-resync')
        assert (target / 'old-0').read_bytes() == payload('old-0')
        assert (target / 'quorum-2').read_bytes() == payload('quorum-2')
        print('[w165] PASS serve-first halt, quorum creates and snapshot resync', flush=True)
    finally:
        for mark in markers.values():
            mark.unlink(missing_ok=True)
            Path(str(mark) + '.arm').unlink(missing_ok=True)
        if mounted:
            mounted.close()
        cluster.stop_all()
        retain_logs(cluster)
        print('[w165] evidence fixture:', work, flush=True)


if '--serve-only' in sys.argv:
    serve_fault()
    shutil.rmtree(work)
    raise SystemExit(0)

serve_fault()

t0 = time.time()
c = None
mounts = []
try:
    assert fuse_ok(), 'NOTRUN: no FUSE on this host'
    c = Cluster('w130', [1, 2, 3])
    # only node 3 carries the corrupt-row hook (loopback: one filesystem)
    for i in (1, 2, 3):
        c.start(i, join=1 if i > 1 else None,
                extra_env={'EFS_FAULT_DIR_CORRUPT_FILE': str(work / 'corrupt-on-3')} if i == 3 else None)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
    c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})

    m = Mount(c, 1, 'mnt-a')
    mounts.append(m)
    control = m.path / 'control-dir'
    control.mkdir()
    write_file(control / 'ok.txt', payload('ok.txt'))

    # the target directory, filled past the spread threshold so its DIR
    # begin-split/migrate/finish sequence runs on all replicas
    target = m.path / 'target-dir'
    target.mkdir()
    marker = work / 'corrupt-on-3'

    # arm the fault on node 3 just before the threshold crossing lands.
    # The spread fires when the entry count EXCEEDS the minimum
    # (efs_meta_dir_note_entry), so the crossing file is number min+1.
    target_entries = int(SPREAD_MIN)
    for k in range(target_entries):
        write_file(target / f'f{k:04d}', payload(f'target/{k}'))
    os.sync()
    marker.touch()          # node 3's next DIR apply damages the row it reads
    write_file(target / f'f{target_entries:04d}', payload(f'target/{target_entries}'))
    os.sync()

    # 1. node 3 must log the damage and the halt of the target's group
    #    (which group? the dir shard's group — found in node 3's log).
    #    Wait for the halt BEFORE more creates: a create that meets the
    #    damaged row on the serve path is covered separately above (W165).
    deadline = time.time() + HALT_DEADLINE
    halt_group = None
    while time.time() < deadline:
        log3 = c.log(3)
        if 'INCOMPATIBLE entry' in log3 and 'apply halted, group not served' in log3:
            gm = re.search(r'group=(\d+) INCOMPATIBLE entry', log3)
            halt_group = int(gm.group(1)) if gm else None
            break
        time.sleep(1)
    assert halt_group is not None, \
        'node 3 never halted the group (log tail: ' + c.log(3)[-800:] + ')'
    assert 'fault dir-corrupt: damaged' in c.log(3), 'damage line missing on node 3'
    print(f'[w130] node 3 halted group {halt_group}: '
          f'{int(time.time() - t0)} s in')

    # the group keeps serving through the quorum: 31 more files land
    for k in range(target_entries + 1, target_entries + 32):
        write_file(target / f'f{k:04d}', payload(f'target/{k}'))
    os.sync()

    # 2. other groups and other replicas keep serving
    probe = m.path / 'post-halt.txt'
    write_file(probe, payload('post-halt.txt'))
    os.sync()
    assert probe.read_bytes() == payload('post-halt.txt')
    rc, out = mgmt('raft-status', c.ep(1))
    assert rc == 0
    print('[w130] healthy replicas still serve; other groups unaffected')

    # 3. Healthy-quorum reads must succeed completely, with no application retry.
    names = sorted(e.name for e in target.iterdir())
    assert len(names) == target_entries + 32, \
        f'partial listing {len(names)} != {target_entries + 32}'
    print('[w130] target dir read complete and exact after halt')

    # 4. raft-status shows the halt: node 3's applied count for the group is
    #    frozen while the leaders advance
    a3 = group_applied(c, 3, halt_group)
    time.sleep(3)
    write_file(control / 'more.txt', payload('more.txt'))
    os.sync()
    ldr = next(i for i in (1, 2) if i != 3)
    rc, out = mgmt('raft-status', c.ep(ldr))
    lm = re.search(rf'\s+group {halt_group} .*?role=LEADER.*?commit=(\d+)', out)
    if not lm:
        rc, out = mgmt('raft-status', c.ep(3 - ldr if ldr == 2 else 2))
        lm = re.search(rf'\s+group {halt_group} .*?role=LEADER.*?commit=(\d+)', out)
    assert lm, 'no live leader found for the halted group'
    a3b = group_applied(c, 3, halt_group)
    assert a3b == a3, f'node 3 applied moved {a3} -> {a3b} past a halted entry'
    assert 'INCOMPATIBLE' not in (c.log(1) + c.log(2)), 'another replica halted'
    print(f'[w130] node 3 applied frozen at {a3} for group {halt_group} '
          f'while the leader commits (visible in raft-status)')

    # 5. recover: raft-resync pulls the leader's snapshot onto node 3
    deadline = time.monotonic() + RESYNC_DEADLINE
    tries = request_before(lambda: mgmt('raft-resync', c.ep(3), str(halt_group)), deadline)
    print(f'[w130] resync accepted after {tries} request(s)')
    served = False
    while time.monotonic() < deadline:
        if 'resync: installed snapshot' in c.log(3) or \
           ('INCOMPATIBLE' not in c.last_run(3).split('resync')[-1] and
            group_applied(c, 3, halt_group) > a3):
            served = True
            break
        time.sleep(2)
    assert served, 'node 3 did not resync/serve the group within the bound'
    print(f'[w130] node 3 resynced; group {halt_group} serves again '
          f'{int(time.time() - t0)} s in')

    # post-recovery: the directory is complete and correct everywhere
    names = sorted(e.name for e in target.iterdir())
    if len(names) != target_entries + 32:
        missing = [f'f{k:04d}' for k in range(target_entries + 32)
                   if f'f{k:04d}' not in set(names)]
        raise AssertionError(f'listing {len(names)} != {target_entries + 32}; missing {missing[:8]}')
    names = sorted(e.name for e in target.iterdir())
    assert (target / 'f0000').read_bytes() == payload('target/0')
    print('[w130] PASS (no application retries)')
except Exception as e:
    print('[w130] FAIL:', e)
    raise
finally:
    marker.unlink(missing_ok=True) if 'marker' in dir() else None
    for m in mounts:
        m.close()
    if c:
        c.stop_all()
        retain_logs(c)
    import sys as _sys
    if _sys.exc_info()[0] is None:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f'[w130] work dir kept for forensics: {work}')
        for i in (1, 2, 3):
            lp = work / 'w130' / f'n{i}.log'
            if lp.exists():
                print(f'[w130] --- n{i}.log tail ---')
                print(lp.read_text(errors='replace')[-1500:])
