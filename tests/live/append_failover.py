#!/usr/bin/env python3
"""K15 live gate for W52 (REPORT after many O_APPENDs) and W64 (append
reservation replay across metadata leader changes).

Starts a private three-daemon TCP cluster (three committed members) on 127.0.0.1,
ports PORT+1..PORT+3, stores under ROOT/n{1,2,3}, mounts one writer client and
one peer client. WRITERS processes each open one shared file O_APPEND (one
persistent descriptor each) and write RECORDS/WRITERS unique 4 KiB records,
then fsync. Every record carries (writer, sequence) and a keyed digest of its
body, so exact-once and corruption checks need no other state.

--failover NAME runs a leadership event loop while the writers run:
  step-down   operator step-down of both metadata groups' leaders (efs-mgmt
              raft-step-down), every --interval seconds
  kill        SIGKILL the daemon leading the file's inode group, restart it
              after --down seconds, every --interval seconds
Writers keep going through the event; a failed write() is recorded as not
acknowledged and its record must never become visible.

--lose-reserve-every N (K15/W64, needs an EFS_FAULTS=1 client build) drops the
reply of every Nth granted O_APPEND reservation on the writer mount, at most
--lose-reserve-max times. The server has committed those reservations; the
client gives up on the write and must abandon the identity under its live
session (cleanup-append), so the reservation retires as one zero hole and
the frontier advances. --peer-writers M runs M extra writers on the peer
mount (a second session) concurrently, which a stranded reservation would
block. Combine with --failover to lose replies across leader changes.

Accounting rule (K15/K29, account() below; tests/test_append_failover_accounting.py):
  * a write the client saw fail (errno) is ACCOUNTED, not a mismatch, when
    its record is absent and every zero hole is explained: an injected lost
    reply leaves exactly one hole; a real failure whose abandon retired the
    reservation (rc=0 retired=1) leaves exactly one; a real failure whose
    abandon retired nothing or did not complete (BUSY) leaves at most one
    (the reservation may never have applied, or session/last-close cleanup
    resolved it); a failure with no abandon (refused before reserving)
    leaves none. Accounted failures and holes are reported separately.
  * Availability is separate: every non-injected write/fsync/close error
    fails the overall gate, even if its absent record and hole are explained.
  * MISMATCH: an acknowledged record missing, duplicated, torn, corrupt or
    out of order; a failed write's record visible; an unknown block; more
    holes than the abandons explain or fewer than the retirements require;
    an injected loss without an rc=0 abandon of its identity; a size that is
    not 4 KiB x (acknowledged + holes).

Checks (each one recorded in the JSON report, the run fails on any miss):
  * final fsync per writer returns 0 within --report-bound seconds
    (W52: the client's REPORT receive bound, EFS_IO_TIMEOUT_MS = 30 s)
  * no REPORT (type 67) receive-timeout retry in the client log
  * A11 finding 2: every REPORT within B = --report-latency-bound seconds
    (3 s, three maximum election timeouts) per leader change it crossed,
    and no exhausted 8 s REPORT budget (`report-loop`). Each successful
    REPORT the client logs from 1 s (`slow-ok type=67 ... us=`) may take
    B x max(1, n), n = the step-down/kill events issued from B before it
    started to its end (step-down mode issues both groups' events 3 s apart,
    so one REPORT can cross two). Recovery-scan resolutions on the daemons
    (`txn-recover ... ->`: a transaction nothing finished for 5 s) are
    counted and listed, not a verdict of their own
  * warm primary, warm peer and cold (all daemons + clients restarted) reads
    contain every acknowledged record exactly once, no unacknowledged record,
    per-writer order preserved, no torn or foreign block, size = 4 KiB x count
  * lost-reply mode: at least one reply was lost; every injected loss was
    abandoned with rc=0; peer writers completed (the frontier was never
    stranded); other failed writes are accounted under the rule above

Raw logs stay under ROOT (outside Git). Usage: see docs/status/k15-w52-w64-20261022.md.
"""
import argparse, calendar, hashlib, json, multiprocessing as mp, os, re, signal, socket
import struct, subprocess, sys, time
from pathlib import Path

REC = 4096
MAGIC = b'K15APPND'

def body(writer, seq):
    head = MAGIC + struct.pack('>II', writer, seq)
    fill = hashlib.shake_256(b'k15-v1' + head).digest(REC - len(head) - 16)
    return head + fill + hashlib.blake2b(head + fill, digest_size=16).digest()

def parse(block):
    if len(block) != REC:
        return ('torn', None)
    if block == bytes(REC):
        return ('hole', None)
    if block[:8] != MAGIC:
        return ('foreign', None)
    writer, seq = struct.unpack('>II', block[8:16])
    if block != body(writer, seq):
        return ('corrupt', (writer, seq))
    return ('ok', (writer, seq))

def scan(path):
    size = path.stat().st_size
    out = dict(size=size, blocks=0, kinds={}, records=[])
    with path.open('rb', buffering=0) as f:
        while True:
            b = f.read(REC)
            if not b:
                break
            kind, ident = parse(b)
            out['kinds'][kind] = out['kinds'].get(kind, 0) + 1
            if ident is not None and kind == 'ok':
                out['records'].append(ident)
            out['blocks'] += 1
    return out

def verify(path, acked, failed, holes=0, holes_max=None):
    """acked/failed: {writer: [seq,...]}. holes..holes_max: zero blocks the
    abandons explain (account()); holes_max defaults to holes. Returns
    (ok, summary)."""
    if holes_max is None:
        holes_max = holes
    s = scan(path)
    seen = {}
    for w, q in s['records']:
        seen.setdefault((w, q), 0)
        seen[(w, q)] += 1
    want = {(w, q) for w, qs in acked.items() for q in qs}
    bad = {(w, q) for w, qs in failed.items() for q in qs}
    dup = sorted(k for k, n in seen.items() if n > 1)
    missing = sorted(want - set(seen))
    unacked_visible = sorted(bad & set(seen))
    unknown = sorted(set(seen) - want - bad)
    order_err = 0
    last = {}
    for w, q in s['records']:
        if w in last and q <= last[w]:
            order_err += 1
        last[w] = q
    other = {k: v for k, v in s['kinds'].items() if k != 'ok'}
    seen_holes = other.pop('hole', 0)
    ok = (not dup and not missing and not unacked_visible and not unknown and
          not order_err and not other and holes <= seen_holes <= holes_max and
          s['size'] == REC * (len(want) + seen_holes) and s['size'] % REC == 0)
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    return ok, dict(size=s['size'], expected_size=REC * (len(want) + seen_holes), blocks=s['blocks'],
                    holes=seen_holes, expected_holes=[holes, holes_max],
                    acknowledged=len(want), visible_records=len(s['records']),
                    duplicates=len(dup), missing=len(missing),
                    unacknowledged_visible=len(unacked_visible), unknown=len(unknown),
                    order_errors=order_err, non_record_blocks=other, sha256=sha,
                    first_missing=missing[:5], first_duplicate=dup[:5],
                    first_unacked_visible=unacked_visible[:5])

ABANDON_RX = re.compile(r'append-abandon ino=\d+ seq=(\d+) len=\d+ rc=(-?\d+) retired=(\d)')
LOST_RX = re.compile(r'append-reserve fault ino=\d+ seq=(\d+) lost-reply=1')

def client_events(text, mount):
    """Injected losses and abandons from one client log."""
    lost = [dict(mount=mount, seq=int(m.group(1))) for m in LOST_RX.finditer(text)]
    ab = [dict(mount=mount, seq=int(m.group(1)), rc=int(m.group(2)), retired=int(m.group(3)))
          for m in ABANDON_RX.finditer(text)]
    return lost, ab

def account(failed, lost, abandons):
    """K15/K29 rule: which failed writes and zero holes are explained.

    failed: {writer: [seq...]} writes that returned an error. lost: injected
    reply losses [{mount, seq}] (seq = append identity). abandons: [{mount,
    seq, rc, retired}] from the client logs. Returns a dict with the hole
    interval the views must fall in, the accounted (non-injected) failures,
    and the protocol mismatches found here (record-level ones are verify's).
    """
    nfailed = sum(len(v) for v in failed.values())
    key = lambda e: (e['mount'], e['seq'])
    by_id = {}
    for e in abandons:
        by_id.setdefault(key(e), []).append(e)
    mismatches = []
    lost_ids = {key(e) for e in lost}
    for e in lost:
        if not any(x['rc'] == 0 for x in by_id.get(key(e), [])):
            mismatches.append(f"injected loss {e['mount']} seq={e['seq']} has no rc=0 abandon")
    real = [x for k, xs in by_id.items() if k not in lost_ids for x in xs[-1:]]
    retired = [x for x in real if x['rc'] == 0 and x['retired']]
    unresolved = [x for x in real if not (x['rc'] == 0 and x['retired'])]
    holes_min = len(lost_ids) + len(retired)
    holes_max = holes_min + len(unresolved)
    if len(lost_ids) + len(real) > nfailed:
        mismatches.append(f'{len(lost_ids) + len(real)} abandoned identities but only {nfailed} failed writes')
    holes_max = min(holes_max, nfailed)
    if holes_min > holes_max:
        mismatches.append(f'retirements need {holes_min} holes, failed writes allow {holes_max}')
    return dict(injected_losses=len(lost_ids), failed_writes=nfailed,
                accounted_failures=nfailed - len(lost_ids),
                accounted_abandons=dict(retired=len(retired),
                                        retired_nothing=sum(1 for x in unresolved if x['rc'] == 0),
                                        not_completed=sum(1 for x in unresolved if x['rc'] != 0),
                                        lines=real[:10]),
                accounted_without_abandon=max(0, nfailed - len(lost_ids) - len(real)),
                holes_min=holes_min, holes_max=holes_max, mismatches=mismatches,
                ok=not mismatches)

def availability(accounting, results):
    """Explained holes preserve integrity, never excuse an application error.

    Injected reserve losses deliberately fail one write each. Every other
    failed write, fsync error or short write fails the availability gate.
    """
    return (accounting['accounted_failures'] == 0 and
            sum(r['error_count'] for r in results) == accounting['failed_writes'])


def writer_main(index, path, count, fsync_every, q, progress_dir=None):
    acked, failed, lat, errors = [], [], [], []
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    t0 = checkpoint = time.monotonic()
    progress = Path(progress_dir) / f'writer-{index}-progress.json' if progress_dir else None
    def note(phase, done):
        if progress:
            tmp = progress.with_suffix('.tmp')
            tmp.write_text(json.dumps(dict(writer=index,phase=phase,completed=done,
                acknowledged=len(acked),failed=len(failed),wall=time.time(),elapsed_s=time.monotonic()-t0)))
            tmp.replace(progress)
    note('writes',0)
    for seq in range(count):
        data = body(index, seq)
        b = time.monotonic()
        try:
            n = os.write(fd, data)
            if n != REC:
                errors.append(f'short write seq={seq} n={n}')
                failed.append(seq)
            else:
                acked.append(seq)
        except OSError as e:
            errors.append(f'write seq={seq} errno={e.errno}')
            failed.append(seq)
        lat.append(time.monotonic() - b)
        if time.monotonic()-checkpoint >= 10:
            note('writes',seq+1)
            checkpoint=time.monotonic()
        if fsync_every and (seq + 1) % fsync_every == 0:
            b = time.monotonic()
            try:
                os.fsync(fd)
            except OSError as e:
                errors.append(f'mid fsync seq={seq} errno={e.errno} s={time.monotonic()-b:.3f}')
    writes = time.monotonic() - t0
    note('final-fsync',count)
    b = time.monotonic()
    fsync_rc = 0
    try:
        os.fsync(fd)
    except OSError as e:
        fsync_rc = e.errno
        errors.append(f'final fsync errno={e.errno}')
    fsync_s = time.monotonic() - b
    try:
        os.close(fd)
    except OSError as e:
        errors.append(f'close errno={e.errno}')
    note('done',count)
    if progress_dir:
        (Path(progress_dir)/f'writer-{index}-latencies.json').write_text(json.dumps(lat))
    lat.sort()
    pct = {str(p): lat[min(len(lat) - 1, int(p / 100 * len(lat)))] for p in (50, 99, 100)} if lat else {}
    q.put(dict(writer=index, acked=acked, failed=failed, write_seconds=writes,
               final_fsync_seconds=fsync_s, final_fsync_errno=fsync_rc,
               write_latency_s=pct, errors=errors[:50], error_count=len(errors)))

class Cluster:
    def __init__(self, a):
        self.a, self.root, self.rel = a, Path(a.root), Path(a.rel)
        self.procs, self.clients = {}, {}
        self.env = dict(os.environ, EFS_TRANSPORT='tcp')
        if a.client_trace:
            self.env['EFS_REPORT_DBG'] = '1'
        (self.root / 'logs').mkdir(parents=True, exist_ok=True)

    def addr(self, i):
        return f'127.0.0.1:{self.a.port + i}'

    def start_node(self, i, join=False):
        """K33 positional CLI: the seed (and every restart) as `efsd
        ADDR:PORT STORAGE`; a first start of node 2/3 joins node 1."""
        d = self.root / f'n{i}'
        d.mkdir(exist_ok=True)
        if i > 1 and join:
            cmd = [self.rel / 'efsd', '--join', self.addr(1), '--listen', self.addr(i), d]
        else:
            cmd = [self.rel / 'efsd', self.addr(i), d]
        cmd += ['--quota', self.a.quota, '--discovery-port', '0']
        log = open(self.root / 'logs' / f'efsd-n{i}.log', 'ab')
        self.procs[i] = subprocess.Popen([str(c) for c in cmd], env=self.env, stdout=log, stderr=log)
        for _ in range(120):
            if self.procs[i].poll() is not None:
                raise RuntimeError(f'efsd n{i} exited rc={self.procs[i].returncode}')
            try:
                socket.create_connection(('127.0.0.1', self.a.port + i), timeout=.2).close()
                return
            except OSError:
                time.sleep(.25)
        raise RuntimeError(f'efsd n{i} not listening')

    def kill_node(self, i, sig=signal.SIGKILL):
        p = self.procs.get(i)
        if p and p.poll() is None:
            p.send_signal(sig)
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()

    def mgmt(self, *args, check=False):
        r = subprocess.run([str(self.rel / 'efs-mgmt'), *map(str, args)], env=self.env,
                           capture_output=True, text=True, timeout=60)
        if check and r.returncode:
            raise RuntimeError(f'efs-mgmt {args}: {r.stdout}{r.stderr}')
        return r

    def mount(self, name, extra_env=None):
        m = self.root / name
        m.mkdir(exist_ok=True)
        log = open(self.root / 'logs' / f'fuse-{name}.log', 'ab')
        label = [self.a.client_label] if self.a.client_label else []
        env = dict(self.env, **(extra_env or {}))
        p = subprocess.Popen([str(self.rel / 'efs-fuse'), self.addr(1), *label, str(m), '-f'],
                             env=env, stdout=log, stderr=log)
        for _ in range(160):
            if p.poll() is not None:
                raise RuntimeError(f'efs-fuse {name} exited')
            if subprocess.run(['mountpoint', '-q', str(m)]).returncode == 0 and \
               subprocess.run(['timeout', '2', 'stat', str(m)], capture_output=True).returncode == 0:
                self.clients[name] = p
                return m
            time.sleep(.25)
        raise RuntimeError(f'mount {name} not serving')

    def unmount(self, name):
        m = self.root / name
        result = subprocess.run([str(self.rel / 'scripts/client.sh'), 'stop', str(m)],
                                capture_output=True, text=True, timeout=660)
        with (self.root/'logs'/'clean-stops.log').open('a') as log:
            log.write(f'client {name} rc={result.returncode}\n{result.stdout}{result.stderr}')
        if result.returncode or os.path.ismount(m):
            raise RuntimeError(f'client {name} clean stop failed; retained: {result.stdout}{result.stderr}')
        p = self.clients.get(name)
        if p:
            p.wait(timeout=30)
            if p.returncode != 0:
                raise RuntimeError(f'client {name} exited {p.returncode}, not a clean cold restart')
            self.clients.pop(name)

    def stop_all(self):
        for name in list(self.clients):
            self.unmount(name)
        for i in list(self.procs):
            p = self.procs[i]
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                p.wait(timeout=90)
            from importlib.util import spec_from_file_location, module_from_spec
            spec=spec_from_file_location('task_n_server_processes',self.rel/'scripts/server_processes.py')
            owner=module_from_spec(spec);spec.loader.exec_module(owner)
            record=owner.read_exit_record(self.root/f'n{i}'/'log/efsd.exit')
            outcome,message=owner.classify(p.pid,record)
            with (self.root/'logs'/'clean-stops.log').open('a') as log:
                log.write(message+'\n')
            if p.returncode != 0 or outcome != 'orderly':
                raise RuntimeError(f'node {i}: {message}; stores retained')
        self.procs.clear()

    def status(self, i):
        """{group: (role, leader_raft_id, term)} as node i sees it."""
        r = self.mgmt('raft-status', self.addr(i))
        out = {}
        for m in re.finditer(r'group (\d+) hosted=1 role=(\w+) leader=(-?\d+) term=(\d+)', r.stdout):
            out[int(m.group(1))] = (m.group(2), int(m.group(3)), int(m.group(4)))
        return out

    def leader_node(self, group):
        """node-id (1-based) currently leading group, by majority view."""
        for i in sorted(self.procs):
            if self.procs[i].poll() is not None:
                continue
            try:
                s = self.status(i).get(group)
            except Exception:
                continue
            if s and s[0].lower().startswith('lead'):
                return i
        return None

def failover_loop(c, a, stop_evt, events):
    t_end = time.monotonic() + a.failover_window
    time.sleep(a.first_event)
    turn = 0
    while not stop_evt.is_set() and time.monotonic() < t_end:
        # step-down: both groups each interval; kill: one leader per
        # interval, alternating group 0 / group 2.
        groups = (0, 2) if a.failover == 'step-down' else ((0, 2)[turn % 2],)
        turn += 1
        for group in groups:
            if stop_evt.is_set():
                break
            node = c.leader_node(group)
            ev = dict(t=time.time(), group=group, leader=node, kind=a.failover)
            if node is None:
                ev['skipped'] = 'no leader observed'
            elif a.failover == 'step-down':
                ev['old_term'] = c.status(node)[group][2]
                r = c.mgmt('raft-step-down', c.addr(node), group)
                ev['rc'] = r.returncode
                ev['out'] = (r.stdout + r.stderr).strip()[-200:]
            elif a.failover == 'kill':
                c.kill_node(node)
                ev['killed'] = node
                time.sleep(a.down)
                c.start_node(node)
                ev['restarted'] = node
            time.sleep(3)
            ev['new_leader'] = c.leader_node(group)
            if ev['new_leader'] is not None:
                ev['new_term'] = c.status(ev['new_leader'])[group][2]
            events.append(ev)
            print('failover', json.dumps(ev), flush=True)
        stop_evt.wait(a.interval)

def grep_count(path, pattern):
    rx = re.compile(pattern)
    n, first = 0, []
    try:
        with open(path, 'r', errors='replace') as f:
            for line in f:
                if rx.search(line):
                    n += 1
                    if len(first) < 5:
                        first.append(line.strip()[:240])
    except FileNotFoundError:
        pass
    return n, first

def formation_root(c):
    """Raw management reads expose NOT_PRIMARY; follow one valid live hint.

    This is readiness discovery, not replay of a failed application write.
    A second redirect or any refusal remains not-ready for the existing
    bounded formation loop. Never infer formation from process exit alone.
    """
    result=c.mgmt('raft-getattr',c.addr(1),1)
    if result.returncode==0 and re.search(r'\bstatus=7\b',result.stdout):
        hint=re.search(r'\bprimary=(\d+)\b',result.stdout)
        if hint and int(hint.group(1)) in (2,3):
            result=c.mgmt('raft-getattr',c.addr(int(hint.group(1))),1)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rel', required=True, type=lambda v: str(Path(v).resolve()),
                   help='build dir with efsd, efs-fuse, efs-mgmt, scripts/client.sh')
    p.add_argument('--root', required=True, help='private fresh directory (e.g. /data1/k15/run1)')
    p.add_argument('--port', type=int, default=18300, help='base port; nodes use PORT+1..PORT+3')
    p.add_argument('--records', type=int, default=20000)
    p.add_argument('--writers', type=int, default=4)
    p.add_argument('--fsync-every', type=int, default=0, help='also fsync every N records per writer')
    p.add_argument('--report-bound', type=float, default=30.0)
    p.add_argument('--report-latency-bound', type=float, default=3.0,
                   help='per-REPORT bound across leader changes (A11 finding 2)')
    p.add_argument('--quota', default='8G')
    p.add_argument('--failover', choices=('none', 'step-down', 'kill'), default='none')
    p.add_argument('--interval', type=float, default=20.0)
    p.add_argument('--first-event', type=float, default=10.0)
    p.add_argument('--failover-window', type=float, default=1e9)
    p.add_argument('--down', type=float, default=5.0)
    p.add_argument('--client-trace', action='store_true')
    p.add_argument('--client-label', default='', help="export label argument for older clients (e.g. 'default')")
    p.add_argument('--label', default='')
    p.add_argument('--lose-reserve-every', type=int, default=0,
                   help='W64: drop every Nth granted reserve reply on the writer mount (EFS_FAULTS=1 build)')
    p.add_argument('--lose-reserve-max', type=int, default=4)
    p.add_argument('--peer-writers', type=int, default=None,
                   help='extra writers on the peer mount (default 1 with --lose-reserve-every, else 0)')
    a = p.parse_args()
    if a.peer_writers is None:
        a.peer_writers = 1 if a.lose_reserve_every else 0
    root = Path(a.root)
    if root.exists() and any(root.iterdir()):
        sys.exit(f'{root} must be empty or absent')
    root.mkdir(parents=True, exist_ok=True)
    assert a.records % (a.writers + a.peer_writers) == 0
    for i in (1, 2, 3):
        if subprocess.run(['ss', '-ltnH', f'sport = :{a.port+i}'], capture_output=True, text=True).stdout.strip():
            sys.exit(f'port {a.port+i} in use')
    c = Cluster(a)
    report = dict(label=a.label, args=vars(a), started=time.time(), checks={}, verdict='FAIL')
    out = root / 'report.json'
    rc = 1
    try:
        for i in (1, 2, 3):
            c.start_node(i, join=True)
            time.sleep(1)
        # K33: the three members form and create the root themselves.
        for _ in range(120):
            r = formation_root(c)
            if r.returncode == 0 and re.search(r'\bstatus=0\b',r.stdout):
                break
            time.sleep(1)
        else:
            raise RuntimeError('cluster never formed: ' + r.stdout + r.stderr)
        fault = {}
        if a.lose_reserve_every:
            fault = dict(EFS_FAULT_APPEND_LOSE_EVERY=str(a.lose_reserve_every),
                         EFS_FAULT_APPEND_LOSE_MAX=str(a.lose_reserve_max))
        mnt = c.mount('mnt', fault)
        peer = c.mount('peer')
        path = mnt / 'append-shared'
        with open(path, 'xb'):
            pass
        q = mp.Queue()
        nw = a.writers + a.peer_writers
        per = a.records // nw
        ws = [mp.Process(target=writer_main,
                         args=(w, str(path if w < a.writers else peer / path.name), per, a.fsync_every, q, str(root)))
              for w in range(nw)]
        events = []
        import threading
        stop_evt = threading.Event()
        t0 = time.monotonic()
        for w in ws:
            w.start()
        ft = None
        failover_errors = []
        def checked_failover():
            try:
                failover_loop(c,a,stop_evt,events)
            except BaseException as exc:
                failover_errors.append(repr(exc))
                print('failover controller failed:',repr(exc),flush=True)
        if a.failover != 'none':
            ft = threading.Thread(target=checked_failover, daemon=True)
            ft.start()
        results = []
        while len(results) < nw:
            try:
                results.append(q.get(timeout=30))
            except Exception:
                dead=[dict(pid=w.pid,exit=w.exitcode) for w in ws if w.exitcode not in (None,0)]
                if dead:
                    raise RuntimeError(f'writer exited without a result: {dead}')
                el = time.monotonic() - t0
                try:
                    sz = path.stat().st_size // REC
                except OSError:
                    sz = -1
                print(f'progress elapsed={el:.0f}s visible_blocks={sz}', flush=True)
                if el > 6 * 3600:
                    raise RuntimeError('writers exceeded 6 h')
        for w in ws:
            w.join(timeout=60)
        stop_evt.set()
        if ft:
            ft.join(timeout=600)
        wall = time.monotonic() - t0
        results.sort(key=lambda r: r['writer'])
        acked = {r['writer']: r['acked'] for r in results}
        failed = {r['writer']: r['failed'] for r in results}
        report['writers'] = [{k: v for k, v in r.items() if k not in ('acked', 'failed')} |
                             dict(acked=len(r['acked']), failed=len(r['failed'])) for r in results]
        report['wall_seconds'] = wall
        report['events'] = events
        report['failover_errors'] = failover_errors
        report['checks']['failover_exercised'] = (a.failover == 'none' or
            (bool(events) and not failover_errors and not ft.is_alive() and all(
                e.get('rc') == 0 and 'rc=0' in e.get('out','') and
                e.get('new_term',0) > e.get('old_term',0)
                if a.failover == 'step-down' else 'restarted' in e
                for e in events)))
        fs = [r['final_fsync_seconds'] for r in results]
        report['checks']['final_fsync_zero'] = all(r['final_fsync_errno'] == 0 for r in results)
        report['checks']['final_fsync_max_s'] = max(fs)
        report['checks']['final_fsync_within_bound'] = max(fs) <= a.report_bound
        flog = root / 'logs' / 'fuse-mnt.log'
        client_logs = [root/'logs'/f'fuse-{name}.log' for name in ('mnt','peer')]
        retries = [grep_count(log,r'inode-rpc: retry type=67 .*why=recv') for log in client_logs]
        n67 = sum(count for count,_lines in retries)
        first67 = [dict(client=log.name,line=line) for log,(_count,lines) in zip(client_logs,retries) for line in lines][:10]
        report['checks']['report_recv_timeouts'] = n67
        report['checks']['report_recv_timeout_lines'] = first67
        ftext = '\n'.join(log.read_text(errors='replace') for log in client_logs if log.exists())
        slow67, over = [], []
        for stamp, us in re.findall(r'^(\S+)Z inode-rpc: slow-ok type=67 .*us=(\d+)', ftext, re.M):
            end = calendar.timegm(time.strptime(stamp[:19], '%Y-%m-%dT%H:%M:%S')) + float('0' + stamp[19:])
            start, us = end - int(us) / 1e6, int(us)
            crossed = sum(1 for e in events if start - a.report_latency_bound <= e['t'] <= end)
            slow67.append(us)
            if us > a.report_latency_bound * 1e6 * max(1, crossed):
                over.append(dict(end=stamp, ms=us // 1000, leader_changes=crossed))
        loops = re.findall(r'.*report-loop .*', ftext)
        report['checks']['report_slow_ok'] = len(slow67)
        report['checks']['report_max_ms'] = max(slow67, default=0) // 1000
        report['checks']['report_budget_exhausted'] = len(loops)
        report['checks']['report_budget_lines'] = [l[:240] for l in loops[:5]]
        report['checks']['report_over_latency_bound'] = over[:10]
        report_lat_ok = not over and not loops
        report['checks']['report_within_latency_bound'] = report_lat_ok
        rec = [grep_count(root / 'logs' / f'efsd-n{i}.log', r'txn-recover .*-> ') for i in (1, 2, 3)]
        report['checks']['recovery_scan_resolutions'] = sum(n for n, _ in rec)
        report['checks']['recovery_scan_lines'] = [l for _, ls in rec for l in ls][:5]
        report['checks']['fsync_split_lines'] = grep_count(flog, r'fsync-split')[1]
        srv = [grep_count(root / 'logs' / f'efsd-n{i}.log', r'report resolve .*rc=') for i in (1, 2, 3)]
        report['checks']['server_report_resolve_retries'] = sum(n for n, _ in srv)
        report['checks']['server_report_resolve_lines'] = [l for _, ls in srv for l in ls][:5]
        texts = {}
        for name in ('mnt', 'peer'):
            try:
                texts[name] = (root / 'logs' / f'fuse-{name}.log').read_text(errors='replace')
            except FileNotFoundError:
                texts[name] = ''
        lost, abandons = [], []
        for name, text in texts.items():
            l, ab = client_events(text, name)
            lost += l
            abandons += ab
        acct = account(failed, lost, abandons)
        report['checks']['accounting'] = acct
        holes, holes_max = acct['holes_min'], acct['holes_max']
        ok_l = acct['ok']
        report['checks']['availability'] = availability(acct,results)
        if a.lose_reserve_every:
            # Each injected loss follows a committed reservation; the frontier
            # must never strand the peer session (W64).
            peer_ok = all(r['final_fsync_errno'] == 0 and len(r['acked']) + len(r['failed']) == per
                          for r in results if r['writer'] >= a.writers)
            ok_l = ok_l and acct['injected_losses'] >= 1 and peer_ok
            report['checks']['lost_reserve'] = dict(
                lost_replies=acct['injected_losses'], first_lost=[e['seq'] for e in lost[:5]],
                peer_writers_ok=peer_ok, ok=ok_l)
        ok_w, report['checks']['warm_primary'] = verify(path, acked, failed, holes, holes_max)
        ok_p, report['checks']['warm_peer'] = verify(peer / path.name, acked, failed, holes, holes_max)
        # Cold: drain clients, stop every daemon, restart, fresh mount.
        c.stop_all()
        time.sleep(2)
        for i in (1, 2, 3):
            c.start_node(i)
            time.sleep(1)
        time.sleep(3)
        mnt = c.mount('mnt-cold')
        ok_c, report['checks']['cold'] = verify(mnt / path.name, acked, failed, holes, holes_max)
        same = len({report['checks'][v]['sha256'] for v in ('warm_primary', 'warm_peer', 'cold')}) == 1
        report['checks']['views_identical'] = same
        all_fsync_ok = report['checks']['final_fsync_zero'] and report['checks']['final_fsync_within_bound']
        report['verdict'] = 'PASS' if (ok_w and ok_p and ok_c and same and ok_l and all_fsync_ok and n67 == 0
                                       and report_lat_ok and report['checks']['availability']
                                       and report['checks']['failover_exercised']) else 'FAIL'
        # Accounted failures do not fail integrity, but do fail availability.
        report['integrity'] = 'PASS' if (ok_w and ok_p and ok_c and same and acct['ok']) else 'FAIL'
        report['accounted_failures'] = acct['accounted_failures']
        rc = 0 if report['verdict'] == 'PASS' else 1
    except Exception as e:
        report['exception'] = repr(e)
        raise
    finally:
        try:
            c.stop_all()
        except Exception as e:
            report['cleanup_error']=repr(e)
            report['verdict']='FAIL'
            rc=1
            print('stop failed, fixture retained:',e,flush=True)
        report['finished'] = time.time()
        out.write_text(json.dumps(report, indent=2, default=str))
        print(json.dumps({k: report.get(k) for k in ('label', 'verdict', 'integrity', 'accounted_failures',
                                                     'wall_seconds', 'checks')}, indent=1, default=str)[:6000], flush=True)
    sys.exit(rc)

if __name__ == '__main__':
    main()
