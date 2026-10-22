#!/usr/bin/env python3
"""K15 live gate for W52 (REPORT after many O_APPENDs) and W64 (append
reservation replay across metadata leader changes).

Starts a private three-daemon TCP cluster (EFS_MD_RAFT_N=3) on 127.0.0.1,
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

Checks (each one recorded in the JSON report, the run fails on any miss):
  * final fsync per writer returns 0 within --report-bound seconds
    (W52: the client's REPORT receive bound, EFS_IO_TIMEOUT_MS = 30 s)
  * no REPORT (type 67) receive-timeout retry in the client log
  * warm primary, warm peer and cold (all daemons + clients restarted) reads
    contain every acknowledged record exactly once, no unacknowledged record,
    per-writer order preserved, no torn or foreign block, size = 4 KiB x count

Raw logs stay under ROOT (outside Git). Usage: see docs/status/k15-w52-w64-20261022.md.
"""
import argparse, hashlib, json, multiprocessing as mp, os, re, signal, socket
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

def verify(path, acked, failed):
    """acked/failed: {writer: [seq,...]}. Returns (ok, summary)."""
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
    ok = (not dup and not missing and not unacked_visible and not unknown and
          not order_err and not other and s['size'] == REC * len(want) and
          s['size'] % REC == 0)
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    return ok, dict(size=s['size'], expected_size=REC * len(want), blocks=s['blocks'],
                    acknowledged=len(want), visible_records=len(s['records']),
                    duplicates=len(dup), missing=len(missing),
                    unacknowledged_visible=len(unacked_visible), unknown=len(unknown),
                    order_errors=order_err, non_record_blocks=other, sha256=sha,
                    first_missing=missing[:5], first_duplicate=dup[:5],
                    first_unacked_visible=unacked_visible[:5])

def writer_main(index, path, count, fsync_every, q):
    acked, failed, lat, errors = [], [], [], []
    fd = os.open(path, os.O_WRONLY | os.O_APPEND)
    t0 = time.monotonic()
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
        if fsync_every and (seq + 1) % fsync_every == 0:
            b = time.monotonic()
            try:
                os.fsync(fd)
            except OSError as e:
                errors.append(f'mid fsync seq={seq} errno={e.errno} s={time.monotonic()-b:.3f}')
    writes = time.monotonic() - t0
    b = time.monotonic()
    fsync_rc = 0
    try:
        os.fsync(fd)
    except OSError as e:
        fsync_rc = e.errno
        errors.append(f'final fsync errno={e.errno}')
    fsync_s = time.monotonic() - b
    os.close(fd)
    lat.sort()
    pct = {str(p): lat[min(len(lat) - 1, int(p / 100 * len(lat)))] for p in (50, 99, 100)} if lat else {}
    q.put(dict(writer=index, acked=acked, failed=failed, write_seconds=writes,
               final_fsync_seconds=fsync_s, final_fsync_errno=fsync_rc,
               write_latency_s=pct, errors=errors[:50], error_count=len(errors)))

class Cluster:
    def __init__(self, a):
        self.a, self.root, self.rel = a, Path(a.root), Path(a.rel)
        self.procs, self.clients = {}, {}
        self.env = dict(os.environ, EFS_MD_RAFT_N='3', EFS_TRANSPORT='tcp')
        if a.client_trace:
            self.env['EFS_REPORT_DBG'] = '1'
        (self.root / 'logs').mkdir(parents=True, exist_ok=True)

    def addr(self, i):
        return f'127.0.0.1:{self.a.port + i}'

    def start_node(self, i, join=True):
        d = self.root / f'n{i}'
        d.mkdir(exist_ok=True)
        cmd = [self.rel / 'efsd', '--node-id', str(i), '--addr', '127.0.0.1',
               '--port', str(self.a.port + i), '--storage', d, '--quota', self.a.quota,
               '--direct-io' if self.a.direct else '--no-direct-io']
        if i > 1 and join:
            cmd += ['--join', self.addr(1)]
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

    def mount(self, name):
        m = self.root / name
        m.mkdir(exist_ok=True)
        log = open(self.root / 'logs' / f'fuse-{name}.log', 'ab')
        label = [self.a.client_label] if self.a.client_label else []
        p = subprocess.Popen([str(self.rel / 'efs-fuse'), self.addr(1), *label, str(m), '-f'],
                             env=self.env, stdout=log, stderr=log)
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
        subprocess.run([str(self.rel / 'scripts/client.sh'), 'stop', str(m)],
                       capture_output=True, timeout=180)
        p = self.clients.pop(name, None)
        if p:
            try:
                p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                p.kill(); p.wait()

    def stop_all(self):
        for name in list(self.clients):
            self.unmount(name)
        for i in list(self.procs):
            self.kill_node(i, signal.SIGTERM)
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

def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--rel', required=True, help='build dir with efsd, efs-fuse, efs-mgmt, scripts/client.sh')
    p.add_argument('--root', required=True, help='private fresh directory (e.g. /data1/k15/run1)')
    p.add_argument('--port', type=int, default=18300, help='base port; nodes use PORT+1..PORT+3')
    p.add_argument('--records', type=int, default=20000)
    p.add_argument('--writers', type=int, default=4)
    p.add_argument('--fsync-every', type=int, default=0, help='also fsync every N records per writer')
    p.add_argument('--report-bound', type=float, default=30.0)
    p.add_argument('--quota', default='8G')
    p.add_argument('--direct', action='store_true')
    p.add_argument('--failover', choices=('none', 'step-down', 'kill'), default='none')
    p.add_argument('--interval', type=float, default=20.0)
    p.add_argument('--first-event', type=float, default=10.0)
    p.add_argument('--failover-window', type=float, default=1e9)
    p.add_argument('--down', type=float, default=5.0)
    p.add_argument('--client-trace', action='store_true')
    p.add_argument('--client-label', default='', help="export label argument for older clients (e.g. 'default')")
    p.add_argument('--label', default='')
    a = p.parse_args()
    root = Path(a.root)
    if root.exists() and any(root.iterdir()):
        sys.exit(f'{root} must be empty or absent')
    root.mkdir(parents=True, exist_ok=True)
    assert a.records % a.writers == 0
    for i in (1, 2, 3):
        if subprocess.run(['ss', '-ltnH', f'sport = :{a.port+i}'], capture_output=True, text=True).stdout.strip():
            sys.exit(f'port {a.port+i} in use')
    c = Cluster(a)
    report = dict(label=a.label, args=vars(a), started=time.time(), checks={}, verdict='FAIL')
    out = root / 'report.json'
    rc = 1
    try:
        for i in (1, 2, 3):
            c.start_node(i)
            time.sleep(1)
        for _ in range(60):
            r = c.mgmt('raft-mkfs', c.addr(1))
            if 'raft-mkfs rc=0 ' in r.stdout:
                break
            time.sleep(1)
        else:
            raise RuntimeError('raft-mkfs failed: ' + r.stdout + r.stderr)
        mnt = c.mount('mnt')
        peer = c.mount('peer')
        path = mnt / 'append-shared'
        with open(path, 'xb'):
            pass
        q = mp.Queue()
        per = a.records // a.writers
        ws = [mp.Process(target=writer_main, args=(w, str(path), per, a.fsync_every, q)) for w in range(a.writers)]
        events = []
        import threading
        stop_evt = threading.Event()
        t0 = time.monotonic()
        for w in ws:
            w.start()
        ft = None
        if a.failover != 'none':
            ft = threading.Thread(target=failover_loop, args=(c, a, stop_evt, events), daemon=True)
            ft.start()
        results = []
        while len(results) < a.writers:
            try:
                results.append(q.get(timeout=30))
            except Exception:
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
        fs = [r['final_fsync_seconds'] for r in results]
        report['checks']['final_fsync_zero'] = all(r['final_fsync_errno'] == 0 for r in results)
        report['checks']['final_fsync_max_s'] = max(fs)
        report['checks']['final_fsync_within_bound'] = max(fs) <= a.report_bound
        flog = root / 'logs' / 'fuse-mnt.log'
        n67, first67 = grep_count(flog, r'inode-rpc: retry type=67 .*why=recv')
        report['checks']['report_recv_timeouts'] = n67
        report['checks']['report_recv_timeout_lines'] = first67
        report['checks']['fsync_split_lines'] = grep_count(flog, r'fsync-split')[1]
        srv = [grep_count(root / 'logs' / f'efsd-n{i}.log', r'report resolve .*rc=') for i in (1, 2, 3)]
        report['checks']['server_report_resolve_retries'] = sum(n for n, _ in srv)
        report['checks']['server_report_resolve_lines'] = [l for _, ls in srv for l in ls][:5]
        ok_w, report['checks']['warm_primary'] = verify(path, acked, failed)
        ok_p, report['checks']['warm_peer'] = verify(peer / path.name, acked, failed)
        # Cold: drain clients, stop every daemon, restart, fresh mount.
        c.stop_all()
        time.sleep(2)
        for i in (1, 2, 3):
            c.start_node(i)
            time.sleep(1)
        time.sleep(3)
        mnt = c.mount('mnt-cold')
        ok_c, report['checks']['cold'] = verify(mnt / path.name, acked, failed)
        all_fsync_ok = report['checks']['final_fsync_zero'] and report['checks']['final_fsync_within_bound']
        report['verdict'] = 'PASS' if (ok_w and ok_p and ok_c and all_fsync_ok and n67 == 0) else 'FAIL'
        rc = 0 if report['verdict'] == 'PASS' else 1
    except Exception as e:
        report['exception'] = repr(e)
        raise
    finally:
        report['finished'] = time.time()
        out.write_text(json.dumps(report, indent=2, default=str))
        print(json.dumps({k: report.get(k) for k in ('label', 'verdict', 'wall_seconds', 'checks')}, indent=1, default=str)[:6000], flush=True)
        try:
            c.stop_all()
        except Exception as e:
            print('stop failed', e)
    sys.exit(rc)

if __name__ == '__main__':
    main()
