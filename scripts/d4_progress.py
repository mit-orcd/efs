#!/usr/bin/env python3
"""Alpha 1.0 checklist D4: bounded progress with packing, `.efs` indexing and
KV maintenance all active, against any running EFS cluster.

    scripts/d4_progress.py --mount /mnt/a [--fg-mount /mnt/b] --seed HOST:PORT \
        [--load-s 600] [--threads 16] [--fg-deadline-s 10] [--drain-s 900] \
        [--client-rss-mib 1024] [--server-rss-mib 4096] [--pack-backlog 200000] \
        [--index-age-s 300] [--host ID=SSH_HOST ...] [--out DIR]
    scripts/d4_progress.py --cluster CLUSTER.json --out DIR    (qualify.py plugin)

It writes only inside its own test directory (created under --mount,
removed at the end unless --keep) and asks the cluster read-only questions
(`efs-mgmt raft-status`, `pack-compact` with 0 segments = summary only,
`.efs/stats --json`). It never changes membership, quotas or configuration.

Load: --threads writers create small files (1 B to 8 KiB, under the packing
threshold) in their own directories through --mount for --load-s seconds;
every eighth create also unlinks an older file of the writer's, so the
packer's segments get holes (compaction work) and GC has work. That keeps
the packer, the `.efs` index refresh (every create is a metadata change)
and the KV memtables/L0 compaction busy.

Foreground, through --fg-mount at the same time, in a loop: write and fsync
a 1 MiB file, read an earlier one back exactly, stat and list a load
directory, run `.efs/stats --json`. Every operation must finish within
--fg-deadline-s; an operation still running at the deadline is a FAIL
(recorded as such, never waited out silently).

Sampled every --sample-s (default 1 s) and recorded with the maximum:
- client RSS of the efs-fuse process of each mount (this host, /proc);
- server RSS of every efsd (/proc on this host, or `ps` over ssh through
  --host ID=SSH_HOST, or the cluster description's node hosts);
- per member, from raft-status: kv_mt_bytes against kv_mem_budget and
  kv_l0_bytes against kv_l0_cap (the K22/W23 design bounds), and the
  largest unapplied Raft backlog;
- pack backlog: candidate-index entries per led group and the oldest
  eligible candidate's lag (pack-compact summary), and files_packed;
- index backlog: `.efs/stats` snapshot pending_changes_at_least, the age
  of the oldest unindexed change (pending_age_ms_at_sample plus the time
  since lag_sample_ns; server wall clock, so on real hosts it assumes
  synchronized clocks) and generation. The refresh runs at most once per
  EFS_QUERY_REFRESH_MS (default 60 s), so --index-age-s must exceed it.

Bounds (PASS needs all): every foreground operation within its deadline;
client and server RSS at or below their bounds; kv_mt_bytes <= kv_mem_budget
and kv_l0_bytes <= kv_l0_cap at every sample; pack backlog at or below
--pack-backlog; index pending age at or below --index-age-s. Progress:
during the load files_packed grows, the index generation advances, and some
member completes a KV memtable flush and a compaction (raft-status
kv_flushes/kv_compactions); after the load stops (the foreground then only
reads and queries) the pack backlog and the index pending changes reach 0
and stay 0 within --drain-s. Every file the load acknowledged and did not unlink, and
every foreground file, then reads back exactly.

A repeated stall is diagnosed, not tolerated: a foreground operation over
its deadline, or no progress while a backlog is non-zero for --stall-s,
fails the run and saves `efs-mgmt raft-status` and the efs-fuse stacks
(/proc/PID/task/*/stack where readable) to DIR/stall-*.txt.

Result: DIR/d4-result.json and, for qualify.py, DIR/plugin-result.json.
Exit 0 PASS, 1 FAIL, 2 usage or cluster unreachable, 77 NOTRUN (no mount).
"""
import argparse, errno, hashlib, json, os, random, re, shlex, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NOTRUN_RC = 77
MIB = 1 << 20


# ---- parsing (pure; tests/test_d4_driver.py) -------------------------------

def parse_raft_bound(text):
    """`efs-mgmt raft-status` -> {'mt', 'budget', 'l0', 'l0_cap', 'flushes',
    'compactions', 'unapplied'}."""
    m = re.search(r'bound kv_mt_bytes=(\d+) kv_mem_budget=(\d+) kv_l0_bytes=(\d+) kv_l0_cap=(\d+) '
                  r'.*?kv_flushes=(\d+) kv_compactions=(\d+)', text)
    if not m:
        return None
    un = [int(x) for x in re.findall(r'\bunapplied=(\d+)', text)]
    return {'mt': int(m.group(1)), 'budget': int(m.group(2)), 'l0': int(m.group(3)),
            'l0_cap': int(m.group(4)), 'flushes': int(m.group(5)), 'compactions': int(m.group(6)),
            'unapplied': max(un) if un else 0}


def parse_pack(text):
    """`efs-mgmt pack-compact SEED 0` -> {'backlog': {group: n}, 'lag_ms': {group: ms},
    'files_packed': total, 'compacted': total, 'put_errors': total, 'led': set}."""
    out = {'backlog': {}, 'lag_ms': {}, 'files_packed': 0, 'compacted': 0, 'put_errors': 0,
           'read_errors': 0, 'led': set()}
    for g, n, lag in re.findall(r'backlog group=(\d+) candidates=(\d+) lag_ms=(\d+)', text):
        out['backlog'][int(g)] = int(n)
        out['lag_ms'][int(g)] = int(lag)
        out['led'].add(int(g))
    for k, key in (('files_packed', 'files_packed'), ('put_errors', 'put_errors'),
                   ('read_errors', 'read_errors')):
        out[key] = sum(int(x) for x in re.findall(rf'\b{k}=(\d+)', text))
    out['compacted'] = sum(int(x) for x in re.findall(r'compacted_total=(\d+)', text))
    return out


def parse_index(text, now_ns=None):
    """`.efs/stats --json` stdout -> the snapshot fields D4 samples.
    pending_age_ms is the age of the oldest unindexed change now: the
    server's age at its last lag sample plus the time since that sample
    (lag_sample_ns, CLOCK_REALTIME on the server; the sample is refreshed
    only when the pending count or the overdue state changes, so the age
    at the sample alone hides a refresh that falls behind). 0 when nothing
    is pending."""
    doc = json.loads(text)
    s = doc['snapshot']
    pending = int(s['pending_changes_at_least'])
    at_sample = int(s['pending_age_ms_at_sample'])
    since = 0
    if pending and now_ns is not None and s.get('lag_sample_ns'):
        since = max(0, now_ns - int(s['lag_sample_ns'])) // 1000000
    return {'generation': int(s['generation']), 'pending': pending,
            'pending_age_ms': at_sample + since if pending else 0, 'pending_age_at_sample_ms': at_sample,
            'error': int(s['refresh_error']), 'overdue': bool(s['refresh_overdue'])}


def rss_kib(status_text):
    m = re.search(r'^VmRSS:\s+(\d+) kB', status_text, re.M)
    return int(m.group(1)) if m else None


def payload(name, size):
    out, k = bytearray(), 0
    while len(out) < size:
        out += hashlib.sha256(f'{name}:{k}'.encode()).digest()
        k += 1
    return bytes(out[:size])


# ---- cluster access ---------------------------------------------------------

class Access:
    def __init__(self, mgmt, seed, hosts):
        self.mgmt, self.seed, self.hosts = mgmt, seed, hosts  # hosts: node id -> ssh host or None

    def run(self, *args, timeout=30):
        try:
            p = subprocess.run([self.mgmt, *args], capture_output=True, text=True, timeout=timeout)
            return p.returncode, p.stdout + p.stderr
        except subprocess.TimeoutExpired:
            return 124, 'timeout'

    def members(self):
        rc, out = self.run('status', self.seed)
        rows = re.findall(r'^\s+node (\d+): (\S+)\s+(up|DOWN)\b', out, re.M)
        return {int(i): ep for i, ep, st in rows}

    def server_rss(self, node, ep):
        """RSS (KiB) of the efsd listening on ep, on its host."""
        port = ep.rsplit(':', 1)[1]
        host = self.hosts.get(node)
        cmd = "ps -eo rss=,args= | awk -v p=':%s' '/[e]fsd/ && index($0, p) {print $1; exit}'" % port
        try:
            if host and host not in ('local', 'localhost'):
                p = subprocess.run(['ssh', '-o', 'BatchMode=yes', host, cmd], capture_output=True,
                                   text=True, timeout=20)
            else:
                p = subprocess.run(['sh', '-c', cmd], capture_output=True, text=True, timeout=20)
            return int(p.stdout.split()[0]) if p.stdout.split() else None
        except (subprocess.TimeoutExpired, ValueError):
            return None


def fuse_pid(mount):
    """The efs-fuse process serving mount (this host), or None."""
    target = os.path.realpath(mount)
    for d in Path('/proc').iterdir():
        if not d.name.isdigit():
            continue
        try:
            argv = (d / 'cmdline').read_bytes().split(b'\0')
        except OSError:
            continue
        if argv and argv[0].endswith(b'efs-fuse') and any(
                a and os.path.realpath(a.decode(errors='replace')) == target for a in argv[1:]):
            return int(d.name)
    return None


def proc_rss(pid):
    try:
        return rss_kib(Path(f'/proc/{pid}/status').read_text())
    except OSError:
        return None


# ---- the run ----------------------------------------------------------------

class Run:
    def __init__(self, a, access):
        self.a, self.x = a, access
        self.out = Path(a.out)
        self.out.mkdir(parents=True, exist_ok=True)
        tag = f'd4-{os.getpid()}-{int(time.time())}'
        self.dir = Path(a.mount) / tag
        self.fg_dir = Path(a.fg_mount) / tag / 'fg'
        self.stop_load = threading.Event()
        self.stop_all = threading.Event()
        self.lock = threading.Lock()
        self.live = {}          # load file -> size (acknowledged, not unlinked)
        self.fg_files = {}      # foreground file -> size
        self.counts = {'created': 0, 'unlinked': 0, 'create_errors': 0}
        self.ops = {}           # kind -> [latencies]
        self.overdue = []       # (kind, seconds) over the deadline
        self.errors = []
        self.samples = []
        self.inflight = {}      # thread name -> (kind, start)
        self.stalls = 0

    # load
    def writer(self, t):
        rnd = random.Random(t)
        d = self.dir / f'w{t}'
        d.mkdir(parents=True, exist_ok=True)
        mine, k = [], 0
        while not self.stop_load.is_set():
            sub = d / f's{k // 1000}'
            if k % 1000 == 0:
                sub.mkdir(exist_ok=True)
            name = sub / f'f{k}'
            size = rnd.randint(1, 8192)
            try:
                with open(name, 'wb') as f:
                    f.write(payload(str(name.relative_to(self.dir)), size))
                with self.lock:
                    self.live[name] = size
                    self.counts['created'] += 1
                mine.append(name)
            except OSError as e:
                with self.lock:
                    self.counts['create_errors'] += 1
                    self.errors.append(f'load create {name}: {e}')
            if k % 8 == 7 and len(mine) > 16:
                victim = mine.pop(rnd.randrange(len(mine) - 8))
                try:
                    os.unlink(victim)
                    with self.lock:
                        self.live.pop(victim, None)
                        self.counts['unlinked'] += 1
                except OSError as e:
                    with self.lock:
                        self.errors.append(f'load unlink {victim}: {e}')
            k += 1

    # foreground
    def timed(self, kind, fn):
        me = threading.current_thread().name
        t = time.time()
        with self.lock:
            self.inflight[me] = (kind, t)
        try:
            fn()
        except (OSError, AssertionError, ValueError, subprocess.SubprocessError) as e:
            with self.lock:
                self.errors.append(f'{kind}: {e}')
        finally:
            dt = time.time() - t
            with self.lock:
                self.inflight.pop(me, None)
                self.ops.setdefault(kind, []).append(dt)
                if dt > self.a.fg_deadline_s:
                    self.overdue.append((kind, round(dt, 2)))

    def foreground(self):
        self.fg_dir.mkdir(parents=True, exist_ok=True)
        k = 0
        while not self.stop_all.is_set():
            name = self.fg_dir / f'g{k}'
            if self.stop_load.is_set():
                # Drain: no new foreground changes, only reads and queries,
                # so the backlogs measured are background work alone.
                old = self.fg_dir / f'g{random.randrange(k)}' if k else None

                def read_only():
                    with self.lock:
                        size = self.fg_files.get(old)
                    if size is not None:
                        assert old.read_bytes() == payload(old.name, size), f'{old} differs'
                self.timed('read 1 MiB', read_only)
                self.timed('.efs/stats', lambda: self.index_sample(record=False))
                time.sleep(self.a.fg_pause_s)
                continue

            def write():
                data = payload(name.name, MIB)
                fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
                try:
                    off = 0
                    while off < len(data):
                        off += os.write(fd, data[off:])
                    os.fsync(fd)
                finally:
                    os.close(fd)
                with self.lock:
                    self.fg_files[name] = MIB
            self.timed('write+fsync 1 MiB', write)
            if k:
                old = self.fg_dir / f'g{random.randrange(k)}'

                def read():
                    with self.lock:
                        size = self.fg_files.get(old)
                    if size is None:
                        return
                    got = old.read_bytes()
                    assert got == payload(old.name, size), f'{old} differs ({len(got)} B)'
                self.timed('read 1 MiB', read)

            def meta():
                d = self.dir / f'w{k % self.a.threads}'
                if d.exists():
                    os.stat(d)
                    sum(1 for _ in os.scandir(d))
            self.timed('stat+readdir', meta)
            self.timed('.efs/stats', lambda: self.index_sample(record=False))
            k += 1
            time.sleep(self.a.fg_pause_s)

    def index_sample(self, record=True):
        p = subprocess.run([sys.executable, str(Path(self.a.fg_mount) / '.efs' / 'stats'), '--json'],
                           capture_output=True, text=True, timeout=self.a.fg_deadline_s * 3,
                           cwd=self.a.fg_mount)
        if p.returncode:
            raise OSError(errno.EIO, f'.efs/stats rc={p.returncode}: {p.stderr.strip()[:200]}')
        return parse_index(p.stdout.strip().splitlines()[-1], time.time_ns())

    # sampling
    def sample(self, phase):
        s = {'t': round(time.time() - self.t0, 2), 'phase': phase}
        s['client_rss_kib'] = {m: proc_rss(pid) for m, pid in self.pids.items()}
        s['nodes'] = {}
        for nid, ep in self.members.items():
            rc, text = self.x.run('raft-status', ep, timeout=20)
            b = parse_raft_bound(text) if rc == 0 else None
            b = dict(b or {}, rss_kib=self.x.server_rss(nid, ep))
            s['nodes'][nid] = b
        rc, text = self.x.run('pack-compact', self.x.seed, '0', timeout=30)
        s['pack'] = parse_pack(text) if rc == 0 else None
        if s['pack']:
            s['pack']['led'] = sorted(s['pack']['led'])
        try:
            s['index'] = self.index_sample()
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as e:
            s['index'] = None
            s['index_error'] = str(e)[:200]
        with self.lock:
            s['live_files'] = len(self.live)
            s['created'] = self.counts['created']
            now = time.time()
            s['inflight_over'] = [(k, round(now - t, 1)) for k, t in self.inflight.values()
                                  if now - t > self.a.fg_deadline_s]
        self.samples.append(s)
        return s

    def stall_dump(self, why):
        self.stalls += 1
        p = self.out / f'stall-{self.stalls}.txt'
        lines = [f'stall: {why}', f't={time.time() - self.t0:.1f}s']
        for nid, ep in self.members.items():
            lines.append(f'--- raft-status node {nid} {ep}')
            lines.append(self.x.run('raft-status', ep, timeout=20)[1])
        for m, pid in self.pids.items():
            lines.append(f'--- efs-fuse {m} pid {pid}')
            for task in sorted(Path(f'/proc/{pid}/task').glob('*')) if pid else []:
                try:
                    lines.append(f'task {task.name}: ' + (task / 'stack').read_text())
                except OSError:
                    try:
                        lines.append(f'task {task.name}: wchan ' + (task / 'wchan').read_text())
                    except OSError:
                        pass
        p.write_text('\n'.join(lines))

    def run(self):
        self.members = self.x.members()
        if not self.members:
            raise SystemExit(2)
        self.pids = {m: fuse_pid(m) for m in sorted({self.a.mount, self.a.fg_mount})}
        self.t0 = time.time()
        self.dir.mkdir(parents=True)
        first = self.sample('before')
        writers = [threading.Thread(target=self.writer, args=(t,), name=f'w{t}', daemon=True)
                   for t in range(self.a.threads)]
        fg = threading.Thread(target=self.foreground, name='fg', daemon=True)
        for w in writers:
            w.start()
        fg.start()
        end = time.time() + self.a.load_s
        last_progress, last_key = time.time(), None
        while time.time() < end:
            s = self.sample('load')
            key = (s['pack'] or {}).get('files_packed'), (s['index'] or {}).get('generation')
            if key != last_key:
                last_progress, last_key = time.time(), key
            elif time.time() - last_progress > self.a.stall_s:
                self.stall_dump('no packer or index progress during the load')
                last_progress = time.time()
            if s['inflight_over']:
                self.stall_dump(f'foreground over its deadline: {s["inflight_over"]}')
            time.sleep(self.a.sample_s)
        self.stop_load.set()
        for w in writers:
            w.join(timeout=self.a.fg_deadline_s * 3)
        load_end = self.sample('load-end')
        # Drain: backlogs reach 0 and stay there for two samples.
        t_stop = time.time()
        drained_at, zero = None, 0
        while time.time() - t_stop < self.a.drain_s:
            s = self.sample('drain')
            pk = s['pack']
            ix = s['index']
            empty = pk is not None and pk['led'] and all(v == 0 for v in pk['backlog'].values()) \
                and ix is not None and ix['pending'] == 0
            zero = zero + 1 if empty else 0
            if zero >= 2:
                drained_at = time.time() - t_stop
                break
            time.sleep(self.a.sample_s)
        self.stop_all.set()
        fg.join(timeout=self.a.fg_deadline_s * 3)
        if fg.is_alive():
            self.overdue.append(('foreground thread', 'still running'))
            self.stall_dump('foreground thread did not finish')
        # Every acknowledged file exact.
        bad = []
        t = time.time()
        for name, size in sorted(self.live.items()):
            try:
                if name.read_bytes() != payload(str(name.relative_to(self.dir)), size):
                    bad.append(str(name))
            except OSError as e:
                bad.append(f'{name}: {e}')
        for name, size in sorted(self.fg_files.items()):
            try:
                if name.read_bytes() != payload(name.name, size):
                    bad.append(str(name))
            except OSError as e:
                bad.append(f'{name}: {e}')
        verify_s = time.time() - t
        return self.verdict(first, load_end, drained_at, bad, verify_s)

    def verdict(self, first, load_end, drained_at, bad, verify_s):
        a, fails = self.a, []
        load = [s for s in self.samples if s['phase'] in ('load', 'load-end')]

        def mx(seq):
            seq = [v for v in seq if v is not None]
            return max(seq) if seq else None
        crss = mx(v for s in self.samples for v in s['client_rss_kib'].values())
        srss = mx(n.get('rss_kib') for s in self.samples for n in s['nodes'].values())
        mt_over = [(s['t'], i, n['mt'], n['budget']) for s in self.samples for i, n in s['nodes'].items()
                   if n.get('mt') is not None and n['mt'] > n['budget']]
        l0_over = [(s['t'], i, n['l0'], n['l0_cap']) for s in self.samples for i, n in s['nodes'].items()
                   if n.get('l0') is not None and n['l0'] > n['l0_cap']]
        backlog = mx(sum(s['pack']['backlog'].values()) for s in self.samples if s['pack'])
        index_age = mx(s['index']['pending_age_ms'] for s in load if s['index'])
        # KV maintenance completed during the load, per member (counters
        # since start: the last load sample minus the first one).
        def delta(key):
            out = {}
            for i in self.members:
                seq = [(s['nodes'].get(i) or {}).get(key) for s in load]
                seq = [v for v in seq if v is not None]
                out[i] = seq[-1] - seq[0] if len(seq) > 1 else 0
            return out
        flushes, comps = delta('flushes'), delta('compactions')
        packed = [s['pack']['files_packed'] for s in load if s['pack']]
        gens = [s['index']['generation'] for s in load if s['index']]
        lat = {k: {'n': len(v), 'max_s': round(max(v), 3), 'p50_s': round(sorted(v)[len(v) // 2], 3)}
               for k, v in self.ops.items() if v}
        m = {'fg_ops': sum(len(v) for v in self.ops.values()), 'fg_latency': lat,
             'fg_overdue': self.overdue, 'client_rss_mib_max': crss and round(crss / 1024, 1),
             'server_rss_mib_max': srss and round(srss / 1024, 1),
             'kv_mt_over_budget': mt_over[:10], 'kv_l0_over_cap': l0_over[:10],
             'kv_mt_max': mx(n.get('mt') for s in self.samples for n in s['nodes'].values()),
             'kv_mem_budget': mx(n.get('budget') for s in self.samples for n in s['nodes'].values()),
             'kv_l0_max': mx(n.get('l0') for s in self.samples for n in s['nodes'].values()),
             'kv_l0_cap': mx(n.get('l0_cap') for s in self.samples for n in s['nodes'].values()),
             'raft_unapplied_max': mx(n.get('unapplied') for s in self.samples for n in s['nodes'].values()),
             'kv_flushes_during_load': flushes, 'kv_compactions_during_load': comps,
             'pack_backlog_max': backlog,
             'pack_lag_ms_max': mx(v for s in self.samples if s['pack'] for v in s['pack']['lag_ms'].values()),
             'files_packed_during_load': packed[-1] - packed[0] if len(packed) > 1 else 0,
             'index_generations_during_load': gens[-1] - gens[0] if len(gens) > 1 else 0,
             'index_pending_age_ms_max': index_age, 'drain_s': drained_at and round(drained_at, 1),
             'load_created': self.counts['created'], 'load_unlinked': self.counts['unlinked'],
             'load_create_errors': self.counts['create_errors'], 'load_live_files': len(self.live),
             'fg_files': len(self.fg_files), 'verify_s': round(verify_s, 1), 'verify_bad': bad[:20],
             'errors': self.errors[:20], 'stalls_dumped': self.stalls, 'samples': len(self.samples)}
        if self.overdue:
            fails.append(f'{len(self.overdue)} foreground operations over {a.fg_deadline_s} s: '
                         f'{self.overdue[:5]}')
        if self.errors:
            fails.append(f'{len(self.errors)} operation errors: {self.errors[:3]}')
        if crss is None or crss > a.client_rss_mib * 1024:
            fails.append(f'client RSS {m["client_rss_mib_max"]} MiB (bound {a.client_rss_mib})')
        if srss is None or srss > a.server_rss_mib * 1024:
            fails.append(f'server RSS {m["server_rss_mib_max"]} MiB (bound {a.server_rss_mib})')
        if mt_over:
            fails.append(f'kv_mt_bytes over kv_mem_budget: {mt_over[:3]}')
        if l0_over:
            fails.append(f'kv_l0_bytes over kv_l0_cap: {l0_over[:3]}')
        if m['kv_mt_max'] is None:
            fails.append('raft-status bound line never sampled')
        if backlog is None or backlog > a.pack_backlog:
            fails.append(f'pack backlog {backlog} (bound {a.pack_backlog})')
        if index_age is None or index_age > a.index_age_s * 1000:
            fails.append(f'index pending age {index_age} ms (bound {a.index_age_s} s)')
        if m['files_packed_during_load'] <= 0:
            fails.append('the packer packed nothing during the load')
        if m['index_generations_during_load'] <= 0:
            fails.append('the .efs index never refreshed during the load')
        if not any(flushes.values()):
            fails.append('no KV memtable flush during the load')
        if not any(comps.values()) and not a.no_l0_check:
            fails.append('no KV compaction during the load')
        if drained_at is None:
            fails.append(f'backlogs not drained within {a.drain_s} s after the load')
        if bad:
            fails.append(f'{len(bad)} acknowledged files not exact: {bad[:3]}')
        if self.counts['created'] < a.threads:
            fails.append(f'load created only {self.counts["created"]} files')
        return ('FAIL' if fails else 'PASS'), fails, m

    def cleanup(self):
        if self.a.keep:
            return
        import shutil
        for d in (self.dir, self.fg_dir.parent):
            shutil.rmtree(d, ignore_errors=True)


def from_cluster(a):
    doc = json.loads(Path(a.cluster).read_text())
    mounts = [m for m in doc.get('mounts', []) if m.get('host', 'local') in ('local', os.uname()[1])]
    if not mounts:
        return None
    a.seed = a.seed or doc['seed']
    a.mount = a.mount or mounts[0]['path']
    a.fg_mount = a.fg_mount or (mounts[1] if len(mounts) > 1 else mounts[0])['path']
    fx = doc.get('fixture', {})
    for k in ('load_s', 'threads', 'fg_deadline_s', 'drain_s', 'client_rss_mib', 'server_rss_mib',
              'pack_backlog', 'index_age_s'):
        if f'd4_{k}' in fx:
            setattr(a, k, type(getattr(a, k))(fx[f'd4_{k}']))
    hosts = {}
    for i, n in enumerate(doc.get('nodes', []), 1):
        hosts[i] = n.get('host')
    src = doc.get('source') or (doc.get('sources') or {}).get(os.uname()[1])
    if src and not a.mgmt:
        a.mgmt = str(Path(src) / 'efs-mgmt')
    return hosts


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cluster')
    p.add_argument('--mount')
    p.add_argument('--fg-mount')
    p.add_argument('--seed')
    p.add_argument('--mgmt')
    p.add_argument('--host', action='append', default=[], help='ID=SSH_HOST for efsd RSS')
    p.add_argument('--out', required=True)
    p.add_argument('--load-s', type=float, default=600.0)
    p.add_argument('--threads', type=int, default=16)
    p.add_argument('--fg-deadline-s', type=float, default=10.0)
    p.add_argument('--fg-pause-s', type=float, default=0.2)
    p.add_argument('--drain-s', type=float, default=900.0)
    p.add_argument('--sample-s', type=float, default=1.0)
    p.add_argument('--stall-s', type=float, default=120.0)
    p.add_argument('--client-rss-mib', type=float, default=1024.0)
    p.add_argument('--server-rss-mib', type=float, default=4096.0)
    p.add_argument('--pack-backlog', type=int, default=200000)
    p.add_argument('--index-age-s', type=float, default=300.0)
    p.add_argument('--no-l0-check', action='store_true',
                   help='do not require a KV compaction during the load (large memtables, short load)')
    p.add_argument('--keep', action='store_true')
    a = p.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    def finish(verdict, reason, metrics=None, rc=None):
        doc = {'verdict': verdict, 'reason': reason, 'fixture': {
            'd4_load_s': a.load_s, 'd4_threads': a.threads, 'd4_fg_deadline_s': a.fg_deadline_s},
            'bounds': {'fg_deadline_s': a.fg_deadline_s, 'client_rss_mib': a.client_rss_mib,
                       'server_rss_mib': a.server_rss_mib, 'pack_backlog': a.pack_backlog,
                       'index_age_s': a.index_age_s, 'drain_s': a.drain_s,
                       'kv': 'kv_mt_bytes <= kv_mem_budget, kv_l0_bytes <= kv_l0_cap'},
            'measured': metrics or {}}
        (out / 'plugin-result.json').write_text(json.dumps(doc, indent=1, default=str))
        (out / 'd4-result.json').write_text(json.dumps(doc, indent=1, default=str))
        print(f'D4 progress: {verdict} {reason}')
        return rc if rc is not None else (0 if verdict == 'PASS' else NOTRUN_RC if verdict == 'NOTRUN' else 1)

    hosts = {}
    if a.cluster:
        hosts = from_cluster(a)
        if hosts is None:
            return finish('NOTRUN', 'no mount on this host in the cluster description')
    for h in a.host:
        k, _, v = h.partition('=')
        hosts[int(k)] = v
    a.fg_mount = a.fg_mount or a.mount
    a.mgmt = a.mgmt or str(ROOT / 'efs-mgmt')
    if not a.mount or not a.seed:
        p.error('--mount and --seed (or --cluster) are required')
    if not os.path.ismount(a.mount) or not os.path.ismount(a.fg_mount):
        return finish('NOTRUN', f'{a.mount} or {a.fg_mount} is not a mount')
    r = Run(a, Access(a.mgmt, a.seed, hosts))
    try:
        verdict, fails, metrics = r.run()
    finally:
        (out / 'd4-samples.json').write_text(json.dumps(r.samples, default=str))
        r.cleanup()
    return finish(verdict, '; '.join(fails), metrics)


if __name__ == '__main__':
    sys.exit(main())
