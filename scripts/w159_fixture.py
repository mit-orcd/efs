#!/usr/bin/env python3
"""W159 reproduction: a private four-member loopback fixture with one FUSE
client on the same host, direct I/O, the way the expiry fixture's closing
POSIX run saw it (intel1 2026-10-23, orcd-mini 2026-10-24).

    scripts/w159_fixture.py --work DIR --out DIR [--port 24510] [--source TREE]
        [--phase core] [--phase loaded] [--repeat 3] [--load-threads 8]
        [--query-refresh-ms 2000] [--cpuset N1,N2,N3,N4,CLIENT]
        [--store ROOT ...] [--perf]

Starts four efsd (K33 CLI: seed + --join, --discovery-port 0, --quota 4G,
EFS_TRANSPORT=tcp, EFS_RAFT_OBS=1) with their stores under --work (or one
--store root per member), forms the cluster with raft-mkfs, mounts one
efs-fuse (-f, EFS_RPC_PROF=1) and then runs:

- phase core: tests/posix/posix_suite.py MOUNT --jobs 1 --timeout-s 30, the
  fixture's own closing run (gc_reclamation.py line ~3019);
- phase loaded: --repeat rounds of the three W159 cases (dir_many_files,
  concurrent_creates_same_dir, dir_times_mkdir_rmdir) while --load-threads
  writers create 1 B..8 KiB files (every eighth create unlinks an older one)
  in their own directories through the same mount.

While a phase runs, `efs-mgmt raft-status` of every member is sampled every
2 s into OUT/raft-status.log. The daemons' and the client's stderr go to
OUT/n1.log..n4.log and OUT/fuse.log. At the end OUT/slow-rpc.txt holds every
`inode-rpc: slow-ok`, `slow-recv` and `retry` line of the client, and
OUT/summary.json the case verdicts, the slowest RPC and the counts.

--query-refresh-ms sets EFS_QUERY_REFRESH_MS for the daemons (the expiry
fixture sets 2000; the product default is 60000); omit it for the default.
--cpuset pins each daemon and the client to a CPU list (taskset -c), for
the "spread over two hosts" comparison. --perf records `perf record -g` of
the leader of group 0 during the loaded phase into OUT/perf-efsd.data.
Exit 0 when every case of every phase passed and no RPC took 1 s or more,
1 otherwise, 2 on a fixture error.
"""
import argparse, json, os, random, re, shutil, signal, socket, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ('dir_many_files', 'concurrent_creates_same_dir', 'dir_times_mkdir_rmdir')


def log(msg):
    print(time.strftime('%H:%M:%S') + ' ' + msg, flush=True)


class Fixture:
    def __init__(self, a):
        self.a = a
        self.src = Path(a.source).resolve()
        self.work = Path(a.work).resolve()
        self.out = Path(a.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.work.mkdir(parents=True, exist_ok=True)
        if str(self.work).startswith('/data1/efs') or str(self.out).startswith('/data1/efs'):
            raise SystemExit('work and out may not live under /data1/efs')
        self.addrs = [f'127.0.0.1:{a.port + i}' for i in range(4)]
        for p in range(a.port, a.port + 4):
            with socket.socket() as s:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                try:
                    s.bind(('127.0.0.1', p))
                except OSError:
                    raise SystemExit(f'port {p} in use')
        roots = [Path(r).resolve() for r in a.store] if a.store else [self.work] * 4
        if len(roots) not in (1, 4):
            raise SystemExit('--store: one root or four')
        if len(roots) == 1:
            roots = roots * 4
        self.stores = [roots[i] / f'n{i + 1}' for i in range(4)]
        self.mount = self.work / 'mnt'
        self.cpusets = a.cpuset.split(',') if a.cpuset else None
        if self.cpusets and len(self.cpusets) != 5:
            raise SystemExit('--cpuset needs five entries: four daemons and the client')
        self.env = dict(os.environ, EFS_TRANSPORT='tcp', EFS_RAFT_OBS='1', TMPDIR=str(self.work / 'tmp'))
        (self.work / 'tmp').mkdir(exist_ok=True)
        if a.query_refresh_ms:
            self.env['EFS_QUERY_REFRESH_MS'] = str(a.query_refresh_ms)
        self.daemons, self.client, self.handles = [], None, []
        self.sampling = False
        self.perf = None
        self.daemon_env = dict(self.env)
        if a.fsync_delay_us is not None:
            # Emulated slow durable barrier in the daemons only (the client
            # and the suite run unchanged): tests/live/w159_slow_fsync.c.
            so = self.work / 'w159_slow_fsync.so'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                            str(self.src / 'tests/live/w159_slow_fsync.c'), '-ldl', '-o', str(so)], check=True)
            self.daemon_env.update(LD_PRELOAD=str(so), EFS_TEST_FSYNC_DELAY_US=str(a.fsync_delay_us))
            if a.fsync_lock:
                self.daemon_env['EFS_TEST_FSYNC_LOCK_FILE'] = str(self.work / 'fsync.lock')

    def taskset(self, i):
        return ['taskset', '-c', self.cpusets[i]] if self.cpusets else []

    def mgmt(self, *args, timeout=30):
        try:
            return subprocess.run([str(self.src / 'efs-mgmt'), *map(str, args)], capture_output=True,
                                  text=True, env=self.env, timeout=timeout)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(args, 124, '', 'timeout')

    def start_daemons(self):
        for i in range(4):
            self.stores[i].mkdir(parents=True, exist_ok=True)
            if i:
                argv = [str(self.src / 'efsd'), '--join', self.addrs[0], '--listen', self.addrs[i], str(self.stores[i])]
            else:
                argv = [str(self.src / 'efsd'), self.addrs[0], str(self.stores[0])]
            argv += ['--discovery-port', '0', '--quota', self.a.quota]
            argv = self.taskset(i) + argv
            lg = open(self.out / f'n{i + 1}.log', 'ab')
            self.handles.append(lg)
            lg.write(f'==== {time.ctime()} {" ".join(argv)}\n'.encode()); lg.flush()
            self.daemons.append(subprocess.Popen(argv, stdout=lg, stderr=subprocess.STDOUT, env=self.daemon_env))
            deadline = time.time() + 30
            while time.time() < deadline:
                if self.daemons[-1].poll() is not None:
                    raise SystemExit(f'efsd n{i + 1} exited rc={self.daemons[-1].returncode}')
                try:
                    with socket.create_connection(('127.0.0.1', self.a.port + i), timeout=0.2):
                        break
                except OSError:
                    time.sleep(0.2)
            time.sleep(1)
        deadline = time.time() + 120
        while time.time() < deadline:
            if 'Cluster nodes (4)' in self.mgmt('status', self.addrs[0]).stdout:
                break
            time.sleep(1)
        else:
            raise SystemExit('members never joined')
        deadline = time.time() + 120
        while time.time() < deadline:
            r = self.mgmt('raft-mkfs', self.addrs[0])
            if r.returncode == 0 and 'raft-mkfs rc=0 ' in r.stdout:
                break
            time.sleep(0.5)
        else:
            raise SystemExit('raft-mkfs never succeeded: ' + r.stdout + r.stderr)
        deadline = time.time() + 120
        while time.time() < deadline:
            if all(self.mgmt('raft-getattr', ad, 1).returncode == 0 for ad in self.addrs):
                log('cluster formed, all members answer raft-getattr')
                return
            time.sleep(1)
        raise SystemExit('members never became ready')

    def mount_client(self):
        self.mount.mkdir(exist_ok=True)
        lg = open(self.out / 'fuse.log', 'ab')
        self.handles.append(lg)
        env = dict(self.env, EFS_RPC_PROF='1')
        argv = self.taskset(4) + [str(self.src / 'efs-fuse'), self.addrs[0], str(self.mount), '-f']
        self.client = subprocess.Popen(argv, stdout=lg, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.client.poll() is not None:
                raise SystemExit('efs-fuse exited')
            if subprocess.run(['mountpoint', '-q', str(self.mount)]).returncode == 0 and \
               subprocess.run(['timeout', '2', 'stat', str(self.mount)], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0:
                log('client serving')
                return
            time.sleep(0.5)
        raise SystemExit('client never served')

    def sampler(self):
        """raft-status of every member, plus the host's load, the CPU time of
        the fixture's own processes and the busiest other processes (another
        tenant of the host is a confound the summary must show)."""
        with open(self.out / 'raft-status.log', 'a') as f, open(self.out / 'host.log', 'a') as hl:
            while self.sampling:
                stamp = time.strftime('%H:%M:%S')
                for ad in self.addrs:
                    r = self.mgmt('raft-status', ad, timeout=10)
                    f.write(f'--- {stamp} {ad}\n{r.stdout}{r.stderr}')
                f.flush()
                try:
                    load = open('/proc/loadavg').read().strip()
                    own = []
                    for name, p in [(f'n{i + 1}', d) for i, d in enumerate(self.daemons)] + [('fuse', self.client)]:
                        if p is None:
                            continue
                        st = open(f'/proc/{p.pid}/stat').read().split(')')[-1].split()
                        own.append(f'{name}={int(st[11]) + int(st[12])}')
                    top = subprocess.run(['ps', '-eo', 'pid,user,pcpu,comm', '--sort=-pcpu'], capture_output=True,
                                         text=True, timeout=10).stdout.splitlines()[1:6]
                    hl.write(f'--- {stamp} load {load} ticks {" ".join(own)}\n' + '\n'.join(top) + '\n')
                    hl.flush()
                except (OSError, subprocess.SubprocessError, IndexError, ValueError):
                    pass
                time.sleep(2)

    def leader_pid(self):
        r = self.mgmt('raft-status', self.addrs[0])
        m = re.search(r'group0=node(\d)', r.stdout)
        if not m or m.group(1) == '0':
            return None
        return self.daemons[int(m.group(1)) - 1].pid

    def posix(self, label, extra, timeout=900):
        tsv = self.out / f'posix-{label}.tsv'
        argv = [sys.executable, str(self.src / 'tests/posix/posix_suite.py'), str(self.mount),
                '--jobs', '1', '--timeout-s', '30', '--results', str(tsv)] + extra
        with open(self.out / f'posix-{label}.out', 'ab') as lg:
            lg.write((' '.join(argv) + '\n').encode())
            rc = subprocess.run(argv, stdout=lg, stderr=subprocess.STDOUT, env=self.env, timeout=timeout).returncode
        rows = []
        if tsv.exists():
            for line in tsv.read_text().splitlines():
                if line.startswith('#') or line.startswith('test\t') or not line.strip():
                    continue
                rows.append(line.split('\t'))
        return rc, rows

    def load_worker(self, idx, stop, counts):
        d = self.mount / 'load' / f't{idx}'
        d.mkdir(parents=True, exist_ok=True)
        rnd = random.Random(idx)
        n = 0
        older = []
        while not stop.is_set():
            p = d / f'f{n:07d}'
            try:
                with open(p, 'wb') as f:
                    f.write(b'x' * rnd.randint(1, 8192))
                older.append(p)
                counts[idx] += 1
                if n % 8 == 7 and len(older) > 16:
                    os.unlink(older.pop(0))
            except OSError as e:
                counts['errors'].append(f'{p}: {e}')
            n += 1

    def run_loaded(self):
        stop = threading.Event()
        counts = {i: 0 for i in range(self.a.load_threads)}
        counts['errors'] = []
        threads = [threading.Thread(target=self.load_worker, args=(i, stop, counts), daemon=True)
                   for i in range(self.a.load_threads)]
        for t in threads:
            t.start()
        time.sleep(5)
        if self.a.perf:
            pid = self.leader_pid()
            if pid:
                self.perf = subprocess.Popen(['perf', 'record', '-F', '499', '-g', '-p', str(pid), '-o',
                                              str(self.out / 'perf-efsd.data')],
                                             stdout=subprocess.DEVNULL, stderr=open(self.out / 'perf.err', 'wb'))
                log(f'perf record on leader pid {pid}')
        results = []
        t0 = time.time()
        for rnd in range(self.a.repeat):
            for case in CASES:
                rc, rows = self.posix(f'loaded-{rnd}-{case}', ['--filter', case])
                results.append(dict(round=rnd, case=case, rc=rc, rows=rows))
                log(f'loaded round {rnd} {case}: rc={rc} {rows}')
        elapsed = time.time() - t0
        if self.perf:
            self.perf.send_signal(signal.SIGINT)
            self.perf.wait(60)
            self.perf = None
        stop.set()
        for t in threads:
            t.join(60)
        created = sum(v for k, v in counts.items() if k != 'errors')
        log(f'load: {created} creates in {elapsed:.0f} s, {len(counts["errors"])} errors')
        return results, dict(creates=created, seconds=elapsed, errors=counts['errors'][:10])

    def stop(self):
        self.sampling = False
        if self.perf:
            self.perf.send_signal(signal.SIGINT); self.perf.wait(60)
        if self.client and self.client.poll() is None:
            subprocess.run(['fusermount3', '-u', str(self.mount)], stderr=subprocess.DEVNULL)
            try:
                self.client.wait(60)
            except subprocess.TimeoutExpired:
                self.client.terminate(); self.client.wait(30)
        for d in self.daemons:
            if d.poll() is None:
                d.terminate()
        for d in self.daemons:
            try:
                d.wait(60)
            except subprocess.TimeoutExpired:
                d.kill(); d.wait()
        for h in self.handles:
            h.close()

    def slow_lines(self):
        pat = re.compile(r'inode-rpc: (slow-ok|slow-recv|retry|shard=)')
        lines = [l for l in (self.out / 'fuse.log').read_text(errors='replace').splitlines() if pat.search(l)]
        (self.out / 'slow-rpc.txt').write_text('\n'.join(lines) + '\n')
        slow_ok = [int(m.group(1)) for l in lines for m in [re.search(r'slow-ok.* us=(\d+)', l)] if m]
        slow_recv = [int(m.group(1)) for l in lines for m in [re.search(r'slow-recv.* us=(\d+)', l)] if m]
        return dict(slow_ok_n=len(slow_ok), slow_ok_max_us=max(slow_ok, default=0),
                    slow_recv_n=len(slow_recv), slow_recv_max_us=max(slow_recv, default=0),
                    retries=sum(1 for l in lines if 'retry' in l))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--source', default=str(ROOT))
    p.add_argument('--port', type=int, default=24510)
    p.add_argument('--quota', default='4G')
    p.add_argument('--phase', action='append', choices=('core', 'loaded'), default=None)
    p.add_argument('--repeat', type=int, default=3)
    p.add_argument('--load-threads', type=int, default=8)
    p.add_argument('--query-refresh-ms', type=int, default=0)
    p.add_argument('--cpuset', default='')
    p.add_argument('--store', action='append', default=None)
    p.add_argument('--perf', action='store_true')
    p.add_argument('--fsync-delay-us', type=int, default=None,
                   help='add this many microseconds to every fsync/fdatasync of the daemons (LD_PRELOAD '
                        'tests/live/w159_slow_fsync.c); 0 only counts them')
    p.add_argument('--fsync-lock', action='store_true',
                   help='with --fsync-delay-us: the four daemons queue their delays on one lock (one disk)')
    p.add_argument('--keep', action='store_true', help='keep the work tree (stores) after the run')
    a = p.parse_args()
    phases = a.phase or ['core', 'loaded']
    fx = Fixture(a)
    summary = dict(source=str(fx.src), port=a.port, phases=phases, query_refresh_ms=a.query_refresh_ms or 60000,
                   cpuset=a.cpuset, stores=[str(s) for s in fx.stores], quota=a.quota,
                   fsync_delay_us=a.fsync_delay_us, fsync_lock=a.fsync_lock, started=time.ctime())
    ok = True
    try:
        fx.start_daemons()
        fx.mount_client()
        fx.sampling = True
        threading.Thread(target=fx.sampler, daemon=True).start()
        if 'core' in phases:
            t0 = time.time()
            rc, rows = fx.posix('core', [])
            fails = [r for r in rows if len(r) > 1 and r[1] not in ('PASS', 'SKIP')]
            summary['core'] = dict(rc=rc, seconds=round(time.time() - t0), cases=len(rows), fails=fails)
            log(f'core: rc={rc} {len(rows)} cases, fails={fails}, {time.time() - t0:.0f} s')
            ok &= rc == 0
        if 'loaded' in phases:
            results, load = fx.run_loaded()
            summary['loaded'] = dict(results=results, load=load)
            ok &= all(r['rc'] == 0 for r in results)
    finally:
        fx.stop()
        summary['rpc'] = fx.slow_lines()
        summary['fsync_calls'] = {}
        for i in range(4):
            for line in (fx.out / f'n{i + 1}.log').read_text(errors='replace').splitlines():
                if line.startswith('slow-fsync: '):
                    summary['fsync_calls'][f'n{i + 1}'] = line.split()[2]
        summary['finished'] = time.ctime()
        ok &= summary['rpc']['slow_ok_max_us'] < 1000000
        summary['verdict'] = 'PASS' if ok else 'FAIL'
        (fx.out / 'summary.json').write_text(json.dumps(summary, indent=2))
        log(f'verdict {summary["verdict"]}: rpc {summary["rpc"]}')
        if not a.keep:
            shutil.rmtree(fx.work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
