#!/usr/bin/env python3
"""AB/AE private loopback measurement. Never targets an existing cluster.

Fixed-rate D4-shaped small-file rewrites plus a 10 Hz stat probe. The pipe
reader drains freely during formation, then pauses or reads slowly for the
measurement window. Recovery drains freely before checked unmount/shutdown.
No failed/late operation is retried or removed from the result.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import k33_daemons as k


def cpu(pid):
    parts = Path('/proc/%d/stat' % pid).read_text().split(') ', 1)[1].split()
    return (int(parts[11]) + int(parts[12])) / os.sysconf('SC_CLK_TCK')


def pct(values, q):
    return sorted(values)[min(len(values)-1, int(len(values)*q))] if values else None


class SinkCluster(k.Cluster):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.mode = 'file'
        self.full_path = Path('/dev/full')
        self.full_files = {}
        self.extra_env = {}
        self.active = threading.Event()
        self.done = threading.Event()
        self.readers = []

    def start(self, i, join=None):
        store = self.dir / ('n%d' % i)
        store.mkdir()
        argv = [str(k.EFSD)]
        argv += [self.ep(i), str(store)] if join is None else [
            '--join', self.ep(join), '--listen', self.ep(i), str(store)]
        argv += ['--quota', '2G', '--writers', '2', '--discovery-port', '0']
        log = open(self.dir / ('n%d.log' % i), 'ab', buffering=0)
        if self.mode in ('blocked', 'slow'):
            r, w = os.pipe()
            fcntl.fcntl(w, getattr(fcntl, 'F_SETPIPE_SZ', 1031), 4096)
            # Linux F_LINUX_SPECIFIC_BASE+7; Python 3.9 omits the name.
            def reader():
                try:
                    while True:
                        if self.active.is_set() and self.mode == 'blocked':
                            self.done.wait(.02)
                            continue
                        if not select.select([r], [], [], .05)[0]:
                            continue
                        data = os.read(r, 256 if self.active.is_set() else 65536)
                        if not data:
                            break
                        log.write(data)
                        if self.active.is_set():
                            self.done.wait(.1)  # <=2560 B/s; then normal drain
                finally:
                    os.close(r)
                    log.close()
            t = threading.Thread(target=reader)
            t.start()
            self.readers.append(t)
            sink = w
        elif self.mode == 'full':
            path = self.full_path / ('n%d.log' % i) if self.full_path.is_dir() else self.full_path
            self.full_files[i] = path
            sink = os.open(path, os.O_WRONLY | os.O_APPEND)
        else:
            sink = log.fileno()
        if self.extra_env:
            trace = self.dir / ('n%d-sync.log' % i)
            argv = ['bash', '-c', 'exec 198>"$1"; shift; exec "$@"',
                    'fsync-trace', str(trace), *argv]
        self.procs[i] = subprocess.Popen(argv, stdout=log, stderr=sink, env=dict(self.env, **self.extra_env))
        if self.mode in ('blocked', 'slow', 'full'):
            os.close(sink)
        if self.mode not in ('blocked', 'slow'):
            log.close()


def require_reporter_startup_failure(binary, store, output):
    """An unsupported stderr sink must refuse startup, not serve without its
    required reporter. eventfd stays open, so FD reuse cannot corrupt a store.
    """
    import ctypes
    store.mkdir()
    lib = ctypes.CDLL(None, use_errno=True)
    fd = lib.eventfd(0, os.O_NONBLOCK | os.O_CLOEXEC)
    assert fd >= 0, ('eventfd', ctypes.get_errno())
    port = k.free_port()
    held = k.hold_port('127.0.0.1', port)
    assert held
    env = {key:value for key,value in os.environ.items() if not key.startswith('EFS_')}
    env['EFS_TRANSPORT'] = 'tcp'
    proc = None
    try:
        with output.open('w') as log:
            proc = subprocess.Popen([str(binary), '127.0.0.1:%d' % port, str(store),
                                     '--quota','1G','--writers','2','--discovery-port','0'],
                                    stdout=log, stderr=fd, env=env)
            try:
                rc = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                raise AssertionError('daemon served without its required telemetry reporter')
            assert rc != 0, ('unsupported telemetry sink accepted', rc)
    finally:
        if proc and proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try: proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        os.close(fd)
        held.close()


def run(a):
    a.output.mkdir(parents=True, exist_ok=False)
    a.work.mkdir(parents=True, exist_ok=False)
    k.EFSD = ROOT / 'tests/faults/efsd'
    env = {key: value for key, value in os.environ.items()
           if not key.startswith('EFS_')}
    env.update(EFS_TRANSPORT=getattr(a, 'transport', 'tcp'), EFS_RAFT_OBS=str(a.obs), EFS_BATCH_OBS='1',
               EFS_PACK_MIN_AGE_MS='1000', EFS_PACK_INTERVAL_MS='200',
               EFS_QUERY_REFRESH_MS='2000')
    if getattr(a, 'rdma_dev', None):
        env['EFS_RDMA_DEV'] = a.rdma_dev
    host = getattr(a, 'listen', '127.0.0.1')
    c = SinkCluster(a.work, 'cluster', (1, 2, 3), env, hosts={i: host for i in (1,2,3)})
    if getattr(a, 'ports', None):
        assert len(set(a.ports)) == 3
        for h in c.held: h.close()
        c.ports = dict(zip((1,2,3), a.ports))
        c.held = [k.hold_port(host, p) for p in a.ports]
        assert all(c.held), 'an explicitly selected private port is unavailable'
    c.mode = a.sink
    c.full_path = Path(getattr(a, 'full_path', '/dev/full'))
    if getattr(a, 'slow_fsync_us', 0):
        so = a.work / 'slow-fsync.so'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                        '-DW159_FSYNC_REPORT_FD=198',
                        str(ROOT/'tests/live/w159_slow_fsync.c'), '-ldl', '-o', str(so)], check=True)
        c.extra_env = dict(LD_PRELOAD=str(so), EFS_TEST_FSYNC_DELAY_US=str(a.slow_fsync_us),
                           EFS_TEST_FSYNC_LOCK_FILE=str(a.work/'fsync.lock'))
    mount = None
    samples, errors, writes, stats = [], [], [], []
    result = dict(sink=a.sink, obs=a.obs, duration_s=a.seconds,
                  rate_per_writer=a.rate, threads=a.threads, verdict='FAIL',
                  full_path=str(c.full_path) if a.sink == 'full' else None,
                  slow_fsync_us=getattr(a, 'slow_fsync_us', 0),
                  transport=env['EFS_TRANSPORT'], rdma_dev=getattr(a, 'rdma_dev', None),
                  versioned=getattr(a, 'versioned', False), endpoints=c.ports)
    try:
        for i in (1, 2, 3):
            c.start(i, None if i == 1 else 1)
        c.wait_members((1, 2, 3), 3)
        c.wait_voters((1, 2, 3), lambda n, gs: set(gs) == {0, 2} and
                      all(hosted and voters == 7 for hosted, voters in gs.values()))
        mount = k.Mount(c, 1, 'mnt')
        fixture = mount.path / 'ab-ae'
        fixture.mkdir()
        paths, expected = [], {}
        versioned = getattr(a, 'versioned', False)
        def payload(w, seq):
            return seq.to_bytes(8, 'big')+bytes([w])*504 if versioned else bytes([w])*512
        for w in range(a.threads):
            d = fixture / ('w%d' % w)
            d.mkdir()
            paths.append(d)
            for n in range(16):
                p = d / str(n)
                p.write_bytes(payload(w, 0))
                expected[p] = payload(w, 0)
        probe = fixture / 'probe'
        probe.write_bytes(b'probe')
        applied_before = {}
        for i in (1,2,3):
            rc, info = k.mgmt(env, 'raft-status', c.ep(i))
            assert rc == 0, ('pre-load status', i, rc)
            (a.output / ('raft-before-n%d.txt' % i)).write_text(info)
            applied_before[i] = sum(map(int, re.findall(r'applied=(\d+)', info)))
        start = time.monotonic()
        drain_seconds = getattr(a, 'drain_seconds', 0)
        total_seconds = drain_seconds + a.seconds
        stop_at = start + total_seconds
        cpus = {i: cpu(p.pid) for i, p in c.procs.items()}
        if not drain_seconds:
            c.active.set()
        def writer(w):
            n = 0
            while time.monotonic() < stop_at:
                t0 = time.monotonic()
                try:
                    p = paths[w] / str(n % 16)
                    with open(p, 'wb', buffering=0) as f:
                        f.write(payload(w, n+1))
                        os.fsync(f.fileno())
                    expected[p] = payload(w, n+1)
                    writes.append((time.monotonic()-start, time.monotonic()-t0))
                except OSError as e:
                    errors.append(dict(kind='write', worker=w, path=str(p), error=repr(e)))
                n += 1
                time.sleep(max(0, start+n/a.rate-time.monotonic()))
        def statter():
            n = 0
            while time.monotonic() < stop_at:
                t0 = time.monotonic()
                try:
                    os.stat(probe)
                    stats.append(time.monotonic()-t0)
                except OSError as e:
                    errors.append(dict(kind='stat', error=repr(e)))
                n += 1
                time.sleep(max(0, start+n*.1-time.monotonic()))
        threads = [threading.Thread(target=writer, args=(w,)) for w in range(a.threads)]
        threads.append(threading.Thread(target=statter))
        for t in threads:
            t.start()
        while time.monotonic() < stop_at:
            if time.monotonic()-start >= drain_seconds:
                c.active.set()
            sample = dict(t=time.monotonic()-start, writes=len(writes), stats=len(stats))
            sample['wchan'] = {}
            for i, proc in c.procs.items():
                sample['wchan'][i] = {}
                for task in Path('/proc/%d/task' % proc.pid).iterdir():
                    try:
                        sample['wchan'][i][task.name] = (task / 'wchan').read_text().strip()
                    except FileNotFoundError:
                        pass
            samples.append(sample)
            time.sleep(.5)
        cpus = {i: cpu(p.pid)-cpus[i] for i, p in c.procs.items()}
        c.active.clear()
        c.done.set()
        if getattr(a, 'full_release', None):
            release = Path(a.full_release).resolve()
            assert release.parent == c.full_path.resolve() and release.name == 'fill'
            release.write_bytes(b'')  # caller-created private tmpfs fill file only
        for t in threads:
            t.join(90)
            assert not t.is_alive(), 'workload did not finish after sink recovery'
        for w in range(a.threads):
            for p in paths[w].iterdir():
                actual = p.read_bytes()
                if actual != expected[p]:
                    errors.append(dict(kind='payload', path=str(p), size=len(actual),
                                       expected_prefix=expected[p][:32].hex(), prefix=actual[:32].hex()))
        time.sleep(6)
        applied_after = {}
        for i in (1,2,3):
            rc, info = k.mgmt(env, 'raft-status', c.ep(i))
            (a.output / ('raft-after-n%d.txt' % i)).write_text(info)
            assert rc == 0, ('post-load status', i, rc)
            applied_after[i] = sum(map(int, re.findall(r'applied=(\d+)', info)))
            assert applied_after[i] > applied_before[i], ('applied prefix did not advance', i)
            rc, info = k.mgmt(env, 'io-stats', c.ep(i))
            (a.output / ('io-n%d.txt' % i)).write_text(info)
            assert rc == 0, ('io-stats', i, rc)
        result.update(verdict='PASS' if not errors and not any(x[1] > 10 for x in writes) else 'FAIL', errors=errors,
                      writes=len(writes), write_ops_s=len([x for x in writes if x[0] <= total_seconds])/total_seconds,
                      write_p50_s=pct([x[1] for x in writes], .5), write_p99_s=pct([x[1] for x in writes], .99),
                      stat_count=len(stats), stat_p50_s=pct(stats, .5), stat_p99_s=pct(stats, .99),
                      daemon_cpu_s=cpus, samples=samples, write_completions=writes,
                      applied_before=applied_before, applied_after=applied_after,
                      drain_seconds=drain_seconds, total_seconds=total_seconds,
                      phases={label:dict(writes=len(ops), ops_s=len(ops)/duration,
                                         write_p99_s=pct([x[1] for x in ops], .99))
                              for label, duration, ops in ([
                                  ('drained', drain_seconds, [x for x in writes if x[0] <= drain_seconds]),
                                  ('controlled', a.seconds, [x for x in writes if drain_seconds < x[0] <= total_seconds])]
                                  if drain_seconds else [('controlled', a.seconds, [x for x in writes if x[0] <= total_seconds])])},
                      late_writes=sum(x[1] > 10 for x in writes), verification='all retained payloads checked; mismatches retained in errors')
        if a.require_progress:
            assert not errors and result['late_writes'] == 0, (
                'foreground failures', len(errors), 'late writes', result['late_writes'])
            assert all(b['writes'] > aa['writes'] for aa, b in zip(samples, samples[10:]) if b['t'] > 10), 'no write progress in 5 s window'
        return result
    except BaseException as e:
        result.update(verdict='FAIL', error=repr(e), errors=errors, samples=samples)
        raise
    finally:
        c.active.clear()
        c.done.set()
        try:
            if mount:
                stop = subprocess.run([str(ROOT / 'scripts/client.sh'), 'stop', str(mount.path)],
                                      text=True, capture_output=True, env=env, timeout=650)
                (a.output / 'mount-stop.txt').write_text(stop.stdout+stop.stderr)
                assert stop.returncode == 0 and not os.path.ismount(mount.path), 'unclean unmount'
                assert mount.p.wait(timeout=30) == 0
                mount.log.close()
            for i, proc in c.procs.items():
                proc.send_signal(signal.SIGTERM)
                assert proc.wait(timeout=60) == 0, ('daemon stop', i)
            for t in c.readers:
                t.join(5)
                assert not t.is_alive(), 'reader did not finish'
        finally:
            for p in c.dir.glob('*.log'):
                text = p.read_bytes()
                i = int(p.stem[1:]) if re.fullmatch(r'n[123]', p.stem) else None
                if i in c.full_files and c.full_files[i].is_file():
                    text += c.full_files[i].read_bytes()
                (a.output / p.name).write_bytes(text)
            # No store deletion here: the caller prunes only after retaining results.
            (a.output / 'result.json').write_text(json.dumps(result, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sink', choices=('file', 'slow', 'blocked', 'full'), default='file')
    p.add_argument('--obs', type=int, choices=(0, 1), default=1)
    p.add_argument('--seconds', type=float, default=35)
    p.add_argument('--drain-seconds', type=float, default=0,
                   help='initial freely drained phase on the same cluster/fixture before controlling the sink')
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--rate', type=float, default=5)
    p.add_argument('--require-progress', action='store_true')
    p.add_argument('--transport', choices=('tcp','rdma'), default='tcp')
    p.add_argument('--rdma-dev')
    p.add_argument('--listen', default='127.0.0.1')
    p.add_argument('--ports', type=int, nargs=3, help='explicit unused private ports for real-HCA runs')
    p.add_argument('--versioned', action='store_true', help='final expected-version payload oracle')
    p.add_argument('--slow-fsync-us', type=int, default=0,
                   help='daemon-only existing W159 test interposer, for wait-log pressure')
    p.add_argument('--full-path', type=Path, default=Path('/dev/full'),
                   help='ENOSPC sink, or a private full-tmpfs directory with n1.log..n3.log')
    p.add_argument('--full-release', type=Path,
                   help='truncate only FULL_PATH/fill after the measurement window to observe recovery')
    print(json.dumps(run(p.parse_args()), default=str)[:1200])
