#!/usr/bin/env python3
"""K9/W87 live append interleavings on loopback daemons, exact record set.

Three efsd on free loopback ports (never 17xxx), stores under TMPDIR. Mount A
runs the fault client tests/append_faults/efs-fuse (EFS_FAULT_APPEND_BARRIER:
one-shot parks at named O_APPEND points); mount B is the normal efs-fuse, a
second session. Writers append 4 KiB records carrying (writer, seq) and a
keyed digest (tests/live/append_failover.py) to one shared file. Each step
forces one interleaving:

  S1 flush racing a new append, one mount: a write parks after its grant
     (stripe held, bytes not cached); a per-record open/write/close writer
     and an fsync on another descriptor run against it.
  S2 leader change between PUT and REPORT, peer waiting: an fsync parks after
     its data PUT; both metadata groups' leaders step down; a mount-B write
     waits on the foreign reservation; then the REPORT goes to the new leader.
  S3 lost reply + leader change mid-reservation: a grant's reply is dropped
     and the write parks; leaders step down; the write fails and abandons
     its identity (one zero hole); later writes on both mounts proceed.
  S4 client restart: mount B unmounts and remounts (new session) and writes.
  S5 leader restart with acknowledged records only in the client cache: the
     group-0 leader stops, leadership moves, it restarts, and the next fsync
     publishes them.
  S6 concurrent coda: two persistent writers, one per-record writer and an
     fsync loop on A, one writer on B.

Every process (efsd, efs-fuse, efs-mgmt) is spawned by a helper forked
before any mount descriptor exists: a fork+exec from this process would
close inherited CLOEXEC descriptors, and each close is a FUSE FLUSH that
waits for the append stripe a parked operation holds.

Checks: every acknowledged record exactly once, the failed one absent, zero
holes equal to the abandons that retired a reservation, per-writer order,
size = 4 KiB x (records + holes); warm through A and B and cold (all daemons
and clients restarted). Skipped (NOTRUN, exit 0) without /dev/fuse,
fusermount3 or the fault client.
"""
import json, os, re, shutil, signal, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests' / 'live'))
import append_failover as af  # noqa: E402

EFSD, MGMT, FUSE = root / 'efsd', root / 'efs-mgmt', root / 'efs-fuse'
FAULT_FUSE = root / 'tests' / 'append_faults' / 'efs-fuse'
for b in (EFSD, MGMT):
    if not b.exists():
        sys.exit(f'{b} missing: run make first')
if not (FUSE.exists() and FAULT_FUSE.exists() and os.access('/dev/fuse', os.R_OK | os.W_OK)
        and shutil.which('fusermount3')):
    print('K9 append interleave live: NOTRUN (no /dev/fuse, fusermount3 or fault client)')
    sys.exit(0)
HOST = '127.0.0.1'
REC = af.REC
work = Path(tempfile.mkdtemp(prefix='efs-k9il-', dir=os.environ.get('TMPDIR')))
barrier = work / 'barrier'
barrier.mkdir()
env = dict(os.environ, EFS_TRANSPORT='tcp', EFS_GC_DISABLE='1')
env.pop('EFS_TEST_MEMBER_EXIT', None)
t0 = time.time()


def say(msg):
    print(f'[{time.time() - t0:6.1f}s] {msg}', flush=True)


HELPER = r'''
import json, os, signal, subprocess, sys, time
procs = {}
for line in sys.stdin:
    r = json.loads(line)
    try:
        if r['op'] == 'run':
            p = subprocess.run(r['argv'], capture_output=True, text=True, env=r['env'], timeout=60)
            out = dict(rc=p.returncode, out=p.stdout + p.stderr)
        elif r['op'] == 'spawn':
            with open(r['log'], 'a') as log:
                p = subprocess.Popen(r['argv'], stdout=log, stderr=subprocess.STDOUT, env=r['env'])
            procs[p.pid] = p
            out = dict(pid=p.pid)
        elif r['op'] == 'poll':
            out = dict(rc=procs[r['pid']].poll())
        elif r['op'] == 'stop':
            p = procs.pop(r['pid'])
            if p.poll() is None:
                p.send_signal(r['sig'])
                try:
                    p.wait(timeout=r.get('timeout', 30))
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
            out = dict(rc=p.returncode)
    except Exception as e:  # noqa: BLE001
        out = dict(error=repr(e))
    sys.stdout.write(json.dumps(out) + '\n')
    sys.stdout.flush()
'''
helper = subprocess.Popen([sys.executable, '-c', HELPER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          text=True, close_fds=True)
helper_mu = threading.Lock()


def ask(**req):
    with helper_mu:
        helper.stdin.write(json.dumps(req) + '\n')
        helper.stdin.flush()
        out = json.loads(helper.stdout.readline())
    if 'error' in out:
        raise RuntimeError(out['error'])
    return out


class Proc:
    """A process the helper owns (efsd or efs-fuse)."""
    def __init__(self, argv, log, extra=None):
        self.pid = ask(op='spawn', argv=[str(a) for a in argv], log=str(log),
                       env=dict(env, **(extra or {})))['pid']
        self.rc = None

    def poll(self):
        if self.rc is None:
            self.rc = ask(op='poll', pid=self.pid)['rc']
        return self.rc

    def stop(self, sig=signal.SIGTERM, timeout=30):
        if self.rc is None:
            self.rc = ask(op='stop', pid=self.pid, sig=int(sig), timeout=timeout)['rc']


def mgmt(*args):
    out = ask(op='run', argv=[str(MGMT), *map(str, args)], env=env)
    return out['rc'], out['out']


class Cluster:
    def __init__(self):
        ports = set()
        while len(ports) < 3:
            with socket.socket() as s:
                s.bind((HOST, 0))
                ports.add(s.getsockname()[1])
        self.ports = dict(zip((1, 2, 3), sorted(ports)))
        self.procs = {}

    def ep(self, i):
        return f'{HOST}:{self.ports[i]}'

    def start(self, i, join=None):
        store = work / f'n{i}'
        store.mkdir(exist_ok=True)
        if join:
            args = [str(EFSD), '--join', self.ep(join), '--listen', self.ep(i), str(store)]
        else:
            args = [str(EFSD), self.ep(i), str(store)]
        args += ['--quota', '2G', '--writers', '2', '--discovery-port', '0', '--no-direct-io']
        with open(work / f'n{i}.log', 'a') as log:
            log.write(f'\n==== start {time.time():.3f}\n')
        self.procs[i] = Proc(args, work / f'n{i}.log')

    def log(self, i):
        return (work / f'n{i}.log').read_text(errors='replace')

    def wait_log(self, i, pattern, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if re.search(pattern, self.log(i).rsplit('==== start', 1)[-1]):  # this run only
                return
            assert self.procs[i].poll() is None, f'node {i} exited\n{self.log(i)[-3000:]}'
            time.sleep(0.05)
        raise AssertionError(f'node {i}: {pattern!r} not seen\n{self.log(i)[-3000:]}')

    def wait_serving(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            if mgmt('raft-getattr', self.ep(1), '1')[0] == 0:
                return
            time.sleep(0.3)
        raise AssertionError('cluster never served getattr')

    def stop(self, i, sig=signal.SIGTERM):
        p = self.procs.pop(i, None)
        if p:
            p.stop(sig)

    def stop_all(self):
        for i in list(self.procs):
            self.stop(i)

    def leaders(self, group):
        """(node, term) pairs that claim leadership of group."""
        out = []
        for i in sorted(self.procs):
            rc, text = mgmt('raft-status', self.ep(i))
            for m in re.finditer(r'group (\d+) hosted=1 role=(\w+) leader=(-?\d+) term=(\d+)', text):
                if int(m.group(1)) == group and m.group(2).lower().startswith('lead'):
                    out.append((i, int(m.group(4))))
        return out

    def leader(self, group, timeout=30, newer_than=-1):
        end = time.time() + timeout
        while time.time() < end:
            ls = [x for x in self.leaders(group) if x[1] > newer_than]
            if len(ls) == 1:
                return ls[0]
            time.sleep(0.2)
        raise AssertionError(f'group {group}: no single leader newer than term {newer_than}')

    def step_down(self, group):
        node, term = self.leader(group)
        rc, out = mgmt('raft-step-down', self.ep(node), group)
        assert rc == 0, out
        new = self.leader(group, newer_than=term)
        say(f'group {group}: leader {node} term {term} -> {new[0]} term {new[1]}')
        return new


class Mount:
    def __init__(self, c, node, name, binary=FUSE, extra=None):
        self.name, self.path = name, work / name
        self.path.mkdir(exist_ok=True)
        self.logp = work / f'{name}.log'
        # Right after a full restart the session can be refused (BUSY) until
        # the groups settle; the client then exits and is started again.
        for attempt in range(10):
            self.p = Proc([binary, c.ep(node), self.path, '-f'], self.logp, extra)
            end = time.time() + 30
            while time.time() < end and not os.path.ismount(self.path) and self.p.poll() is None:
                time.sleep(0.1)
            if os.path.ismount(self.path):
                return
            assert self.p.poll() is not None and attempt < 9, self.log()[-2000:]
            time.sleep(2)

    def log(self):
        return self.logp.read_text(errors='replace')

    def close(self):
        ask(op='run', argv=['fusermount3', '-u', str(self.path)], env=env)
        end = time.time() + 60
        while time.time() < end and self.p.poll() is None:
            time.sleep(0.1)
        if self.p.poll() is None:
            self.p.stop(signal.SIGKILL)
            ask(op='run', argv=['fusermount3', '-uz', str(self.path)], env=env)


class Writer:
    """One record stream. persist: one O_APPEND descriptor; else open/close per record."""
    def __init__(self, wid, path, persist=True):
        self.id, self.path, self.persist = wid, path, persist
        self.next, self.acked, self.failed, self.errors = 0, [], [], []
        self.fd = os.open(path, os.O_WRONLY | os.O_APPEND) if persist else None

    def put(self):
        seq = self.next
        self.next += 1
        fd = self.fd if self.persist else os.open(self.path, os.O_WRONLY | os.O_APPEND)
        try:
            n = os.write(fd, af.body(self.id, seq))
            assert n == REC, f'short write {n}'
            self.acked.append(seq)
        except OSError as e:
            self.failed.append(seq)
            self.errors.append((seq, e.errno))
        finally:
            if not self.persist:
                os.close(fd)  # close(2) errors propagate: never silent
        return seq

    def fsync(self):
        if self.persist:
            os.fsync(self.fd)

    def close(self):
        if self.persist and self.fd is not None:
            os.fsync(self.fd)
            os.close(self.fd)
            self.fd = None


def arm(point):
    (barrier / f'{point}-entered').unlink(missing_ok=True)
    (barrier / f'{point}-arm').touch()


def wait_parked(point, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        if (barrier / f'{point}-entered').exists():
            return
        time.sleep(0.01)
    raise AssertionError(f'barrier {point} never reached')


def release(point):
    (barrier / f'{point}-arm').unlink(missing_ok=True)


def run(fn, *args):
    out = {}

    def body():
        try:
            out['value'] = fn(*args)
        except BaseException as e:  # noqa: BLE001
            out['error'] = e
    t = threading.Thread(target=body, daemon=True)
    t.start()
    return t, out


def join(t, out, timeout=60):
    t.join(timeout)
    assert not t.is_alive(), 'thread stuck'
    if 'error' in out:
        raise out['error']
    return out.get('value')


def blocked(t, seconds=0.5):
    t.join(seconds)
    return t.is_alive()


c = None
mounts = {}
ok = False
try:
    c = Cluster()
    c.start(1)
    c.start(2, join=1)
    c.start(3, join=1)
    for i in (1, 2, 3):
        c.wait_log(i, r'raft-host: formed: 3 members')
    c.wait_serving()
    a = mounts['a'] = Mount(c, 1, 'mnt-a', FAULT_FUSE, dict({'EFS_FAULT_APPEND_BARRIER': str(barrier)},
                                                     **({'EFS_DCACHE_TRACE': '1'} if os.environ.get('K9_TRACE') else {})))
    b = mounts['b'] = Mount(c, 3, 'mnt-b')
    name = 'append-shared'
    with open(a.path / name, 'xb'):
        pass
    pa, pb = str(a.path / name), str(b.path / name)
    w0 = Writer(0, pa)                  # A, persistent
    w1 = Writer(1, pa, persist=False)   # A, open/write/close per record
    w2 = Writer(2, pb)                  # B, persistent
    writers = [w0, w1, w2]
    for _ in range(4):
        w0.put()
    w0.fsync()
    for _ in range(4):
        w2.put()
    w2.fsync()
    say('warm-up: 8 records')

    # S1: flush racing a new append on one mount. While the granted write is
    # parked (stripe held), a per-record writer, an fsync of another
    # descriptor and a read-only open/close (a FLUSH with nothing of its own)
    # run; each may wait for the stripe, none may lose or misplace a record.
    def side_fsync():
        fd = os.open(pa, os.O_WRONLY | os.O_APPEND)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def ro_close():
        os.close(os.open(pa, os.O_RDONLY))
    arm('reserved')
    t1, o1 = run(w0.put)
    wait_parked('reserved')
    side = [run(w1.put), run(side_fsync), run(ro_close)]
    time.sleep(0.3)
    release('reserved')
    join(t1, o1)
    for t, o in side:
        join(t, o)
    w0.fsync()
    assert not w0.failed and not w1.failed, (w0.errors, w1.errors)
    say('S1 flush racing a new append: done')

    # S2: leader change between PUT and REPORT, peer waiting.
    w0.put()
    arm('published')
    t1, o1 = run(w0.fsync)
    wait_parked('published')
    t2, o2 = run(w2.put)                 # BUSY on the foreign OPEN reservation
    for g in (0, 2):
        c.step_down(g)
    assert blocked(t2, 0.2), 'peer append did not wait for the unpublished reservation'
    release('published')
    join(t1, o1)
    join(t2, o2)
    w2.fsync()
    assert not w0.failed and not w2.failed, (w0.errors, w2.errors)
    say('S2 leader change between PUT and REPORT: done')

    # S3: lost reply, leader change mid-reservation, abandon.
    arm('lose')
    t1, o1 = run(w0.put)
    wait_parked('lose')
    lost_seq = w0.next - 1
    for g in (0, 2):
        c.step_down(g)
    release('lose')
    join(t1, o1)
    assert w0.failed == [lost_seq], (w0.failed, w0.errors)
    w0.put()
    w0.fsync()
    w2.put()
    w2.fsync()
    assert len(w0.failed) == 1 and not w2.failed, (w0.errors, w2.errors)
    abandons = re.findall(r'append-abandon ino=\d+ seq=\d+ len=\d+ rc=(-?\d+) retired=(\d)', a.log())
    assert abandons and all(rc == '0' for rc, _ in abandons), abandons
    say(f'S3 lost reply across leader change: abandons {abandons}')

    # S4: client restart (new session on B).
    w2.close()
    b.close()
    b = mounts['b'] = Mount(c, 2, 'mnt-b')
    w2.path = pb
    w2.fd = os.open(pb, os.O_WRONLY | os.O_APPEND)
    for _ in range(3):
        w2.put()
    w2.fsync()
    say('S4 client restart: done')

    # S5: stop the group-0 leader (SIGTERM, a clean stop) while acknowledged
    # records are only in the client cache, let leadership move, restart it,
    # then fsync: the records publish through the new leader. A SIGKILL is
    # not used: after an unclean stop the store refuses COMMIT until verified
    # recovery (fail-closed, store.c), so no fsync can succeed afterwards.
    for _ in range(3):
        w0.put()
    node, term = c.leader(0)
    c.stop(node, signal.SIGTERM)
    say(f'S5 stopped node {node} (group 0 leader, term {term})')
    c.leader(0, timeout=60, newer_than=term)
    c.start(node)
    c.wait_log(node, r'raft-host: up raft_id=\d+ n=3 formed')
    w0.fsync()
    w1.put()
    assert len(w0.failed) == 1 and not w1.failed, (w0.errors, w1.errors)
    say('S5 leader restart with cached acknowledged records: done')

    # S6: concurrent coda.
    w3 = Writer(3, pa)
    w4 = Writer(4, pb)
    writers += [w3, w4]
    stop = threading.Event()

    def burst(w, n):
        for _ in range(n):
            w.put()
        w.fsync()

    def fsync_loop():
        fd = os.open(pa, os.O_WRONLY | os.O_APPEND)
        try:
            while not stop.is_set():
                os.fsync(fd)
                time.sleep(0.01)
        finally:
            os.close(fd)
    ts = [run(burst, w, 24) for w in (w0, w1, w3)] + [run(fsync_loop)]
    for w_t in ts[:3]:
        join(*w_t, timeout=180)
    stop.set()
    join(*ts[3])
    burst(w4, 8)
    say('S6 concurrent coda: done')

    for w in writers:
        w.close()
    errors = {w.id: w.errors for w in writers if w.errors}
    assert list(errors) == [0] and len(errors[0]) == 1, errors
    acked = {w.id: w.acked for w in writers}
    failed = {w.id: w.failed for w in writers}
    retired = sum(1 for rc, r in re.findall(r'append-abandon ino=\d+ seq=\d+ len=\d+ rc=(-?\d+) retired=(\d)',
                                            a.log()) if r == '1')
    total = sum(len(v) for v in acked.values())
    views = {}
    for label, m in (('warm-a', a), ('warm-b', b)):
        good, views[label] = af.verify(m.path / name, acked, failed, retired)
        assert good, (label, views[label])
    for m in list(mounts.values()):
        m.close()
    mounts.clear()
    c.stop_all()
    for i in (1, 2, 3):
        c.start(i)
    # A simultaneous restart waits out each daemon's rejoin attempt (K33).
    c.wait_serving(timeout=240)
    cold = mounts['cold'] = Mount(c, 1, 'mnt-cold')
    good, views['cold'] = af.verify(cold.path / name, acked, failed, retired)
    assert good, ('cold', views['cold'])
    assert len({v['sha256'] for v in views.values()}) == 1, views
    say(f'verified warm A, warm B, cold: {total} acknowledged records, 1 failed write, '
        f'{retired} retired hole(s), sha256 {views["cold"]["sha256"][:16]}')
    ok = True
finally:
    for f in barrier.glob('*-arm'):
        f.unlink()  # never leave a writer parked behind a failed check
    for m in list(mounts.values()):
        try:
            m.close()
        except Exception as e:  # noqa: BLE001
            print('unmount failed', e)
    if c:
        c.stop_all()
    helper.stdin.close()
    helper.wait(timeout=30)
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f'logs kept under {work}')
print('K9 append interleave live (flush race, PUT/REPORT leader change, lost reply + '
      'leader change, client restart, leader restart, concurrent coda): '
      + ('PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
