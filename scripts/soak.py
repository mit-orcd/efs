#!/usr/bin/env python3
"""Seeded E4/D4 endurance rehearsal. Own loopback processes only; never a live cluster.

python3 scripts/soak.py --work ~/efs-taskD/run --out ~/efs-artifacts/task-d/run --hours 4
The 15-minute recipe uses --seconds 900 --profile rehearsal: five clean
returns, one crash, one client restart and both additions, not four-hour evidence. SIGKILL preserves the
kernel page cache; it is NOT a power-loss test. Stop churn at the first failure,
preserve logs, and refuse to reuse either output directory. No extra EFS syncs:
fsync belongs only to the workload's application acknowledgement contract.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import re
import signal
import subprocess
import sys
import threading
import time
import traceback

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from k33_daemons import Cluster, Mount, EFSD, MGMT, fuse_ok, payload, hold_port
sys.path.insert(0, str(ROOT / 'tests/live'))
import append_failover as APP


def schedule(seconds, seed=12345, scale=1):
    """Distinct clean-stop victims; no simultaneous node/client outages."""
    rng = random.Random(seed)
    events = []
    victims = list(range(1, 6))
    rng.shuffle(victims)
    k = 0
    for at in range(600, int(seconds / scale) + 1, 600):
        if at * scale >= seconds:
            break
        # Offset hourly events to avoid collisions with clean stops.
        events.append(dict(at=at * scale, kind='clean', victim=victims[k % 5]))
        k += 1
    for hour in range(1, int(seconds / (3600 * scale)) + 1):
        for offset, kind, victim in ((-120, 'crash', victims[(hour - 1) % 5]),
                                     (-60, 'client', (hour - 1) % 2)):
            at = (hour * 3600 + offset) * scale
            if at < seconds:
                events.append(dict(at=at, kind=kind, victim=victim))
    for at, member in ((4500, 6), (8100, 7)):
        if at * scale < seconds:
            events.append(dict(at=at * scale, kind='add', victim=member))
    return sorted(events, key=lambda e: e['at'])


def rehearsal_schedule(seed):
    victims=[e['victim'] for e in schedule(3600,seed) if e['kind']=='clean'][:5]
    return ([dict(at=60+105*k,kind='clean',victim=v) for k,v in enumerate(victims)]
            + [dict(at=585,kind='crash',victim=victims[0]),
               dict(at=675,kind='client',victim=0),dict(at=735,kind='add',victim=6),
               dict(at=795,kind='add',victim=7)])


def protection(text):
    def number(rx):
        m = re.search(rx, text, re.M)
        return int(m.group(1)) if m else None
    return dict(chunks=number(r'^Degraded chunks: (\d+)'),
                segments=number(r'^Degraded segments: (\d+)'),
                lost=number(r'^Degraded segments: .*?unrecoverable=(\d+)'),
                down=number(r'^Cluster state: .*?\(\d+ up, (\d+) down\)'),
                orphans=number(r'^Abandoned uploads: pending=(\d+)'),
                full='Protection: full' in text, ok='Cluster state: OK ' in text)


def clean_protection(p, orphans_revoked=False):
    return (all(p.get(k)==v for k,v in dict(chunks=0, segments=0, lost=0, down=0, full=True, ok=True).items())
            and (not orphans_revoked or p.get('orphans')==0))


def recovery_clean(text):
    # Cumulative missing/marked/rebuilt counters need not reset to zero. Current
    # fence, root error, unresolved work and unrecoverable references must clear.
    return ('recovery verified' in text and 'recovery uncertain:' not in text
            and ' ERROR(' not in text and not re.search(
                r'\b(?:unrecoverable|rebuild_waiting|waiting)=[1-9]\d*', text))



def foreign_jobs(owned=()):
    """Read only our UID's /proc entries. Never signal jobs we did not create."""
    allowed = set(owned)
    pid = os.getpid()
    while pid > 1:
        allowed.add(pid)
        try:
            text = Path(f'/proc/{pid}/stat').read_text()
            pid = int(text.rsplit(')', 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    found = []
    for path in Path('/proc').glob('[0-9]*'):
        if int(path.name) in allowed:
            continue
        try:
            if path.stat().st_uid != os.getuid():
                continue
            comm = (path/'comm').read_text().strip()
            args = (path/'cmdline').read_bytes().decode(errors='replace').split('\0')
            runner = len(args)>1 and Path(args[1]).name=='make_test_summary.sh'
            if comm in ('make','gmake','efsd') or runner:
                found.append(dict(pid=int(path.name), command=args[:3], comm=comm))
        except (OSError, ValueError):
            continue
    return found


def owned_session(text):
    found=re.findall(r'^session uuid=([0-9a-f]{32}) epoch=(\d+)$',text,re.M)
    assert len(found)==1, 'cannot identify exact owned mount session'
    return found[0][0],int(found[0][1])


def digest(path):
    h = hashlib.sha256()
    size = 0
    with open(path, 'rb', buffering=0) as f:
        while True:
            b = f.read(1024 * 1024)
            if not b:
                break
            size += len(b)
            h.update(b)
    return dict(size=size, sha256=h.hexdigest())


def io(req):
    """One bounded child operation. Parent owns the acknowledgement journal."""
    path = Path(req['mount']) / req['name']
    op = req['op']
    if op == 'verify':
        got = digest(path)
        assert got == req['expected'], (req['name'], got, req['expected'])
        return got
    if op == 'append-verify':
        ok, doc = APP.verify(path, {int(w): seq for w, seq in req['acked'].items()}, {})
        assert ok, doc
        return doc
    if op == 'query':
        p = subprocess.run([sys.executable, str(Path(req['mount']) / '.efs' / 'stats'), '--json'],
                           capture_output=True, text=True, timeout=30)
        # Catalog initialization is documented ENODATA; parent gives it a bound.
        assert p.returncode == 0, (p.returncode, p.stdout, p.stderr)
        return json.loads(p.stdout)
    if op == 'append':
        b = APP.body(req['writer'], req['seq'])
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            assert os.write(fd, b) == len(b), 'short append (never replay)'
            os.fsync(fd)
        finally:
            os.close(fd)
        return dict(writer=req['writer'], seq=req['seq'])
    if op == 'truncate':
        fd = os.open(path, os.O_RDWR)
        try:
            os.ftruncate(fd, req['size'])
            os.fsync(fd)
        finally:
            os.close(fd)
        expected = dict(size=req['size'], sha256=hashlib.sha256(payload(req['token'],req['size'])).hexdigest())
        assert digest(path) == expected, 'truncate changed the retained prefix'
        return expected
    data = payload(req['token'], req['size'])
    flags = os.O_WRONLY | os.O_CREAT
    if op == 'create':
        flags |= os.O_EXCL
    fd = os.open(path, flags, 0o600)
    try:
        pos = 0
        while pos < len(data):
            n = os.write(fd, data[pos:])
            assert n > 0, 'zero write'
            pos += n
        os.ftruncate(fd, len(data))
        os.fsync(fd)
    finally:
        os.close(fd)
    return dict(size=len(data), sha256=hashlib.sha256(data).hexdigest())


class Soak:
    def __init__(self, a):
        self.a = a
        self.work, self.out = a.work.resolve(), a.out.resolve()
        for path in (self.work, self.out):
            if path.exists():
                raise ValueError(f'refusing existing directory: {path}')
            if '/data1/efs' in str(path):
                raise ValueError('reserved infrastructure path')
            path.mkdir(parents=True)
        (self.work / '.nomedia').touch()
        self.mu = threading.RLock()
        self.mount_mu = [threading.Lock(), threading.Lock()]
        self.stop = threading.Event()
        self.errors, self.manifest, self.appends = [], {}, {0: [], 1: []}
        self.tokens = {}
        self.procs = set()
        self.events = []
        self.mounts = []
        self.started = time.monotonic()
        self.phase = 'setup'
        self.churning = False
        self.quiet_start = None
        self.quiet_files = []
        self.rss_fp = None
        self.monitor_stop = threading.Event()
        self.c = None
        self.log = open(self.out / 'timeline.jsonl', 'a', buffering=1)

    def note(self, kind, **fields):
        with self.mu:
            self.log.write(json.dumps(dict(t=round(time.monotonic()-self.started, 3),
                                           epoch=time.time(), kind=kind, **fields)) + '\n')

    def command(self, argv, timeout=60, label='command', stdin=None):
        begin = time.monotonic()
        crossed_churn = self.churning
        with subprocess.Popen(argv, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                              start_new_session=True, env=self.env) as p:
            with self.mu:
                self.procs.add(p)
            try:
                text, _ = p.communicate(stdin, timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                text, _ = p.communicate()
                self.note('timeout', label=label, argv=[str(x) for x in argv], output=text[-5000:])
                raise AssertionError(f'{label}: deadline {timeout}s exceeded')
            finally:
                with self.mu:
                    self.procs.discard(p)
        self.note('command', label=label, rc=p.returncode, seconds=time.monotonic()-begin,
                  argv=[str(x) for x in argv], output=text, churn_at_start=crossed_churn, churn_at_end=self.churning)
        return p.returncode, text

    def op(self, mount, req, timeout=None):
        req = dict(req, mount=str(self.mounts[mount].path))
        deadline = timeout or (self.a.churn_deadline if self.churning else self.a.io_deadline)
        rc, text = self.command([sys.executable, str(Path(__file__).resolve()), '--io'],
                                deadline, label=req['op'], stdin=json.dumps(req))
        assert rc == 0, f'{req}: child exit {rc}: {text[-3000:]}'
        return json.loads(text.strip().splitlines()[-1])

    def mgmt(self, cmd, node=None):
        node = node or next(i for i in sorted(self.c.procs) if self.c.alive(i))
        return self.command([str(MGMT), cmd, self.c.ep(node)], 60, label=f'{cmd}-n{node}')

    def quiet(self, begin):
        with self.mu:
            self._quiet(begin)

    def _quiet(self, begin):
        if self.rss_fp:
            self.rss_fp.close()
            self.rss_fp = None
        if begin:
            name = self.out / f'rss-quiet-{len(self.quiet_files):03}.txt'
            self.quiet_files.append(name)
            self.rss_fp = open(name, 'a', buffering=1)

    def sample(self):
        with self.mu:
            procs = [p for p in self.c.procs.values() if p.poll() is None]
            procs += [m.p for m in self.mounts if m.p.poll() is None]
            points = []
            for p in procs:
                try:
                    text = Path(f'/proc/{p.pid}/status').read_text()
                    rss = re.search(r'^VmRSS:\s+(\d+) kB', text, re.M)
                    assert rss, f'RSS absent for owned PID {p.pid}'
                    points.append(f'{p.pid}:{rss.group(1)}')
                except FileNotFoundError:
                    raise AssertionError(f'owned process died unexpectedly: {p.pid}')
            if self.rss_fp:
                self.rss_fp.write(f'{int(time.time())} ' + ' '.join(points) + '\n')
            self.note('rss', values=points)

    def start_node(self, i, **kwargs):
        with self.mu:
            return self.c.start(i, **kwargs)

    def stop_node(self, i, sig):
        with self.mu:
            return self.c.stop(i, sig)

    def restart_node(self, i, **kwargs):
        with self.mu:
            return self.c.restart(i, **kwargs)

    def unrelated_jobs(self):
        # The OS process can appear before Cluster.start registers its PID.
        # Serialize birth/death with the ownership snapshot and /proc scan;
        # never whitelist a command path or ignore an unowned process.
        with self.mu:
            return foreign_jobs(p.pid for p in self.c.procs.values())

    def monitor(self, mode):
        try:
            while not self.monitor_stop.is_set():
                if mode == 'rss':
                    self.sample()
                else:
                    jobs = self.unrelated_jobs()
                    assert not jobs, f'quiet host lost to unrelated jobs: {jobs}'
                    self.status()
                    for node in list(self.c.procs):
                        if self.c.alive(node):
                            rc, text = self.mgmt('sync-status', node)
                            if not self.churning:
                                assert rc == 0 and 'recovery uncertain:' not in text and ' ERROR(' not in text, text
                self.monitor_stop.wait(10 if mode == 'rss' else 30)
        except BaseException:
            self.errors.append(traceback.format_exc())
            self.stop.set()
            self.note('monitor-failure', trace=self.errors[-1])

    def status(self, require_clean=False, orphans_revoked=False):
        rc, text = self.mgmt('status')
        p = protection(text)
        self.note('status', rc=rc, protection=p)
        assert p['lost'] in (None, 0), f'lost segments: {p}'
        if require_clean:
            return rc == 0 and clean_protection(p, orphans_revoked)
        if not self.churning and not require_clean:
            assert rc == 0 and clean_protection(p), f'not protected outside churn: {p}'
        return False

    def settle(self, recovery=None, orphans_revoked=False):
        start = time.monotonic()
        while time.monotonic()-start < self.a.recovery_bound:
            if self.stop.is_set():
                raise AssertionError('workload failed while waiting for recovery')
            ready = self.status(require_clean=True, orphans_revoked=orphans_revoked)
            if recovery is not None:
                rc, text = self.mgmt('sync-status', recovery)
                ready = ready and rc == 0 and recovery_clean(text)
            if ready:
                self.note('settled', seconds=time.monotonic()-start, recovery=recovery)
                return
            time.sleep(2)
        raise AssertionError(f'protection/recovery not clean within {self.a.recovery_bound}s')

    def worker(self, idx):
        rng = random.Random(self.a.seed + idx)
        seq = 0
        try:
            while not self.stop.is_set():
                with self.mount_mu[idx]:
                    if self.stop.is_set():
                        break
                    slot = seq % self.a.slots
                    name = f'soak-{idx}-{slot:04}'
                    old = self.manifest.get(name)
                    # Fixed working set; every cycle changes data but not live-file count.
                    large = slot % 64 == 0
                    size = (1, 4, 16, 64)[(slot // 64) % 4] * 1048576 if large else rng.choice((1, 512, 4096, 8192))
                    if old and seq % 5 == 0:
                        req = dict(op='truncate', name=name, size=old['size']//2, token=self.tokens[name])
                    else:
                        req = dict(op='overwrite' if old else 'create', name=name,
                                   token=f'{self.a.seed}:{idx}:{seq}', size=size)
                    got = self.op(idx, req)
                    with self.mu:
                        self.manifest[name] = got
                        self.tokens[name] = req['token']
                        self.note('ack-fsync', mount=idx, name=name, expected=got, request=req)
                    if seq % 8 == 0:
                        self.op(idx, dict(op='append', name='soak-shared', writer=idx, seq=seq))
                        with self.mu:
                            self.appends[idx].append(seq)
                            self.note('ack-append-fsync', writer=idx, seq=seq)
                    if seq % 32 == 31 and self.commands_enabled:
                        self.op(idx, dict(op='query', name=''), 30)
                    # Metadata deadline separate from data: child runs stat/list as a
                    # distinct bounded operation (included in deterministic unit tests).
                    self.op(idx, dict(op='metadata', name=name), self.a.meta_deadline if not self.churning else self.a.churn_deadline)
                seq += 1
                self.stop.wait(self.a.pace)
        except BaseException:
            with self.mu:
                self.errors.append(traceback.format_exc())
            self.stop.set()
            self.note('first-failure', worker=idx, trace=self.errors[-1])

    def verify(self):
        # Freeze only workload operations, never daemons or maintenance. An exact
        # manifest snapshot cannot race the overwrite it is checking.
        with self.mount_mu[0], self.mount_mu[1]:
            manifest = dict(self.manifest)
            assert manifest, 'no acknowledged files'
            for mount in range(2):
                for name, expected in sorted(manifest.items()):
                    self.op(mount, dict(op='verify', name=name, expected=expected))
                self.op(mount, dict(op='append-verify', name='soak-shared', acked=self.appends))
            self.note('verified', files=len(manifest), mounts=2,
                      appends=sum(len(s) for s in self.appends.values()))
            (self.out / 'manifest.json').write_text(json.dumps(dict(files=manifest, appends=self.appends), indent=2))

    def churn(self, event):
        self.quiet(False)
        self.churning = True
        began = time.monotonic()
        self.note('churn-start', event=event)
        kind, victim = event['kind'], event['victim']
        if kind == 'client':
            # Other mount continues writing; this mount drains before killing its
            # FUSE process. This tests session revoke/remount, not replay of an
            # application call interrupted by SIGKILL (covered by K15/K29 gates).
            with self.mount_mu[victim]:
                m = self.mounts[victim]
                uuid,epoch=owned_session(m.logp.read_text())
                m.p.kill()
                m.p.wait(timeout=30)
                rc, text = self.command(['fusermount3', '-u', str(m.path)], 30, 'dead-client-unmount')
                assert rc == 0, text
                m.log.close()
                # D31 deliberately requires operator revocation after client
                # death. Revoke this exact owned UUID, never infer an expiry.
                rc,text=self.command([str(MGMT),'raft-session',self.c.ep(1),'fence',uuid],
                                     self.a.recovery_bound,'revoke-crashed-client')
                assert rc==0 and re.search(r'epoch=%d\b' % (int(epoch)+1),text),text
                # Preserve every client run in a separate log and mount path.
                self.mounts[victim] = Mount(self.c, 1+victim, f'mnt-{victim}-r{len(self.events)}')
        elif kind == 'add':
            self.start_node(victim, join=1, quota=self.a.quota)
            self.c.wait_members(list(range(1, victim+1)), victim, timeout=self.a.recovery_bound)
        else:
            assert all(self.c.alive(i) for i in self.c.procs), 'second member outage refused'
            self.stop_node(victim, signal.SIGKILL if kind == 'crash' else signal.SIGTERM)
            assert all(self.c.alive(i) for i in self.c.procs), 'unexpected second member down'
            self.stop.wait(self.a.down_seconds)
            # Restore the one stopped node even if the workload failed: preserve
            # its recovery evidence, but never launch another scheduled event.
            self.restart_node(victim, quota=self.a.quota)
            self.c.wait_log(victim, r'node \d+ listening on ', timeout=self.a.recovery_bound)
        self.settle(victim if kind == 'crash' else None)
        self.churning = False
        row = dict(event, actual_at=began-self.load_start, seconds=time.monotonic()-began)
        self.events.append(row)
        self.note('churn-end', event=row)
        self.quiet(True)

    def plateau(self):
        self.quiet(False)
        rows = []
        for path in self.quiet_files:
            lines = path.read_text().splitlines()
            if len(lines) < 4:
                rows.append(dict(path=path.name, verdict='NOTRUN', reason='quiet stretch shorter than four RSS samples'))
                continue
            rc, text = self.command([sys.executable, str(ROOT/'scripts/rss_plateau.py'), str(path)], 30, 'rss-plateau')
            rows.append(dict(path=path.name, verdict='PASS' if rc == 0 else 'FAIL', report=text))
        (self.out/'plateau.json').write_text(json.dumps(rows, indent=2))
        assert rows and all(r['verdict']=='PASS' for r in rows), rows

    def run(self):
        self.env = {k: v for k,v in os.environ.items() if not k.startswith(('EFS_', 'LD_PRELOAD'))}
        # Product defaults, only the required transport selected. No new EFS knobs.
        self.env['EFS_TRANSPORT'] = 'tcp'
        self.c = Cluster(self.work, 'cluster', list(range(1,8)), self.env)
        for held in self.c.held:
            held.close()
        self.c.held = []
        self.c.ports = {i: self.a.port_base+i-1 for i in range(1,8)}
        for port in self.c.ports.values():
            held = hold_port('127.0.0.1', port)
            if held is None:
                for h in self.c.held:
                    h.close()
                raise ValueError(f'private port occupied: {port}')
            self.c.held.append(held)
        self.threads = []
        result = dict(verdict='FAIL', source=self.a.source, schedule=(rehearsal_schedule(self.a.seed) if self.a.profile=='rehearsal' else schedule(self.a.seconds,self.a.seed,self.a.schedule_scale)),
                      config=vars(self.a).copy(), scope='one physical host, TCP, O_DIRECT, fault daemon, packing built in; SIGKILL not power loss')
        try:
            for binary in (EFSD, MGMT, ROOT/'efs-fuse'):
                rc, version = self.command([str(binary), '--version'], 10, 'build-identity')
                assert rc == 0, version
                result.setdefault('versions', {})[binary.name] = version.strip()
            for i in (1,2,3):
                self.start_node(i, join=None if i==1 else 1, quota=self.a.quota)
            for i in (1,2,3):
                self.c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
            for i in (4,5):
                self.start_node(i, join=1, quota=self.a.quota)
                self.c.wait_members(list(range(1,i+1)), i, timeout=self.a.recovery_bound)
            self.mounts = [Mount(self.c,1,'mnt-0'), Mount(self.c,2,'mnt-1')]
            self.settle()
            rc, text = self.command([str(MGMT), 'commands-policy', self.c.ep(1), 'get'], 60, 'commands-policy')
            assert rc == 0, text
            policy = re.search(r'mask=(\d+)', text)
            assert policy, text
            self.commands_enabled = bool(int(policy.group(1)))
            result['commands_enabled'] = self.commands_enabled
            if self.commands_enabled:
                # ENODATA is readiness, not a generic retry of a failing test.
                end = time.monotonic()+120
                for mount in self.mounts:
                    while True:
                        rc, text = self.command([sys.executable, str(mount.path/'.efs/stats'), '--json'], 30, 'query-ready')
                        if rc == 0:
                            json.loads(text.strip().splitlines()[-1])
                            break
                        assert rc == 75 and 'not indexed yet' in text and time.monotonic()<end, text
                        time.sleep(2)
            self.phase = 'warmup'
            for i in range(2):
                t = threading.Thread(target=self.worker, args=(i,), daemon=True)
                self.threads.append(t)
                t.start()
            # Fill the fixed working set before measuring a plateau; otherwise
            # legitimate initial cache population is mislabelled a slow leak.
            warm_end=time.monotonic()+180
            while len(self.manifest)<self.a.slots*2:
                assert not self.stop.is_set(), self.errors
                assert time.monotonic()<warm_end, 'fixed working set warmup exceeded 180s'
                time.sleep(.2)
            self.note('warmup-complete', files=len(self.manifest))
            self.phase='load'
            self.load_start=time.monotonic()
            self.quiet(True)
            pending = list(result['schedule'])
            for mode in ('rss', 'status'):
                monitor = threading.Thread(target=self.monitor, args=(mode,), daemon=True)
                self.threads.append(monitor)
                monitor.start()
            verify_at = self.a.verify_seconds
            while time.monotonic()-self.load_start < self.a.seconds:
                assert not self.stop.is_set(), self.errors
                elapsed = time.monotonic()-self.load_start
                if pending and elapsed >= pending[0]['at']:
                    self.churn(pending.pop(0))
                if elapsed >= verify_at:
                    self.verify()
                    verify_at = elapsed + self.a.verify_seconds
                self.stop.wait(.25)
            self.stop.set()
            self.monitor_stop.set()
            for t in self.threads:
                t.join(timeout=self.a.churn_deadline+5)
                assert not t.is_alive(), 'worker failed to finish'
            assert not self.errors, self.errors
            assert not pending, f'unexecuted events: {pending}'
            # No extra events launched after the declared duration.
            self.phase='final'
            self.verify()
            self.quiet(False)
            self.stop.clear()
            # Live sessions can legitimately retain orphan tickets. Collection
            # is required after their documented revocation, never from age.
            for idx,m in enumerate(self.mounts):
                m.close()
                self.mounts[idx]=Mount(self.c,1+idx,f'mnt-final-{idx}')
            self.settle(orphans_revoked=True)
            self.verify()
            self.stop.set()
            self.sample()
            self.plateau()
            result.update(verdict='PASS', files=len(self.manifest), events=self.events,
                          reason='every acknowledged current version and append exact on both mounts; all protection/recovery bounds and RSS checks passed')
        except BaseException:
            result['reason'] = traceback.format_exc()
            self.note('failure', trace=result['reason'], phase=self.phase)
        finally:
            self.stop.set()
            self.monitor_stop.set()
            self.quiet(False)
            self.monitor_stop.set()
            for t in self.threads:
                t.join(timeout=self.a.churn_deadline+5)
            # Only owned children. Capture before teardown; do not remove evidence.
            with self.mu:
                for p in list(self.procs):
                    if p.poll() is None:
                        os.killpg(p.pid, signal.SIGKILL)
            for m in self.mounts:
                m.close()
            self.c.stop_all()
            result.update(seconds=time.monotonic()-self.started, completed_events=self.events)
            # Keep text evidence outside the disposable runtime stores.
            logs = self.out/'logs'
            logs.mkdir(exist_ok=True)
            for path in self.c.dir.glob('*.log'):
                shutil.copyfile(path, logs/path.name)
            self.log.close()
            if result['verdict'] == 'PASS':
                edelete = Path.home()/'bin/edelete'
                if edelete.exists():
                    cleanup = subprocess.run([str(edelete), '--force', '--delete', str(self.work)],
                                             capture_output=True, text=True)
                    (self.out/'cleanup.txt').write_text(cleanup.stdout+cleanup.stderr)
                    if cleanup.returncode:
                        result.update(verdict='FAIL', reason='owned runtime cleanup failed; evidence preserved')
                else:
                    result.update(verdict='FAIL', reason='required ~/bin/edelete missing; runtime retained')
            (self.out/'result.json').write_text(json.dumps(result, indent=2, default=str))
            (self.out/'plugin-result.json').write_text(json.dumps(dict(verdict=result['verdict'], reason=result['reason'][-1000:], fixture=dict(seconds=self.a.seconds, source=self.a.source))))
        print(json.dumps(result, default=str))
        return 0 if result['verdict']=='PASS' else 1


def main(argv=None):
    if argv is None and sys.argv[1:] == ['--io']:
        req=json.load(sys.stdin)
        if req['op']=='metadata':
            path=Path(req['mount'])/req['name']
            print(json.dumps(dict(size=path.stat().st_size, count=len(os.listdir(req['mount'])))))
        else:
            print(json.dumps(io(req)))
        return 0
    p=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--hours', type=float, default=4)
    p.add_argument('--seconds', type=float)
    p.add_argument('--seed', type=int, default=12345)
    p.add_argument('--source', required=True, help='frozen tested commit; recorded in result')
    p.add_argument('--profile', choices=('endurance','rehearsal'), default='endurance')
    p.add_argument('--schedule-scale', type=float, default=1)
    p.add_argument('--verify-seconds', type=float, default=900)
    p.add_argument('--recovery-bound', type=float, default=300)
    p.add_argument('--down-seconds', type=float, default=5)
    p.add_argument('--io-deadline', type=float, default=60)
    p.add_argument('--meta-deadline', type=float, default=10)
    p.add_argument('--churn-deadline', type=float, default=180)
    p.add_argument('--slots', type=int, default=256)
    p.add_argument('--pace', type=float, default=.1)
    p.add_argument('--quota', default='8G')
    p.add_argument('--port-base', type=int, default=19820)
    a=p.parse_args(argv)
    a.seconds=a.seconds if a.seconds is not None else a.hours*3600
    for key in ('seconds','schedule_scale','verify_seconds','recovery_bound','down_seconds','io_deadline','meta_deadline','churn_deadline','slots'):
        if getattr(a,key)<=0:
            p.error(f'{key} must be positive')
    if a.profile=='rehearsal' and a.seconds!=900:
        p.error('rehearsal is exactly 900 seconds, with its own declared event schedule')
    if not 19000 <= a.port_base <= 19993:
        p.error('private port base must be 19000..19993')
    if not fuse_ok():
        print('NOTRUN: requires /dev/fuse, fusermount3 and efs-fuse')
        return 77
    if not all(b.exists() for b in (EFSD,MGMT)):
        p.error('build all and tests/faults/efsd first')
    jobs=foreign_jobs()
    if jobs:
        print('NOTRUN: quiet host required; unrelated jobs: '+json.dumps(jobs))
        return 77
    return Soak(a).run()


if __name__=='__main__':
    sys.exit(main())
