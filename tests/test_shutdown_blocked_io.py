#!/usr/bin/env python3
"""K1/T2: a forced efsd stop exits within its bound even when the exit
record or stderr cannot be written.

After the shutdown deadline efsd writes the exit record and its stderr
lines on helper threads and waits at most EFS_SHUTDOWN_REPORT_MS (5 s) for
them before _exit(3). Runs tests/shutdown_faults/efsd (EFS_FAULTS build;
`make tests/shutdown_faults/efsd`, or EFS_SHUTDOWN_FAULT_BINARY=<path>) as a
private single-node daemon on a disposable root with the packer parked
(EFS_FAULT_SHUTDOWN_PARK=packer) and the total deadline shortened to
TOTAL_MS, and stops it through scripts/server_processes.py stop-file as
stop.sh does:

1. exit record parked before its write, then before its fsync
   (EFS_FAULT_SHUTDOWN_RECORD_PARK=write|fsync, a stalled storage root):
   efsd still exits rc 3 within TOTAL_MS + 5 s; no record from this PID
   exists, so the stop script reports "outcome unknown", rc 4, never
   forced or orderly.
2. stderr a pipe nobody drains, filled before the stop: efsd still exits
   rc 3 within the same bound, after waiting out the report bound for the
   blocked stderr line; the record is written, so the stop script reports
   forced, rc 3.

Before T2 every case hangs in the exit path until the stop script's 60 s
wait gives up (negative control: run one case per argument, e.g.
`EFS_SHUTDOWN_FAULT_BINARY=<pre-fix daemon> tests/test_shutdown_blocked_io.py fsync`).
"""
from pathlib import Path
import os, shutil, socket, subprocess, sys, tempfile, threading, time

root = Path(__file__).resolve().parents[1]
binary = Path(os.environ.get('EFS_SHUTDOWN_FAULT_BINARY') or root/'tests/shutdown_faults/efsd')
helper = root/'scripts/server_processes.py'
TOTAL_MS = 6000
REPORT_MS = 5000
SLACK_MS = 4000  # process exit (threads in kernel I/O), pidfd wake, Python start-up
BOUND_MS = TOTAL_MS + REPORT_MS + SLACK_MS

if not binary.exists():
    sys.exit(f'{binary} missing: run `make tests/shutdown_faults/efsd` first')

storage_tmp = os.environ.get('EFS_TEST_STORAGE_TMPDIR') or None
work = Path(tempfile.mkdtemp(prefix='efs-shutdown-blocked-', dir=storage_tmp))
print('work:', work, 'binary:', binary, flush=True)


def free_ports(n):
    """n distinct free ports: the probes stay open together until all are
    chosen, so concurrently run cases never pick the same one."""
    probes = [socket.socket() for _ in range(n)]
    try:
        for probe in probes:
            probe.bind(('127.0.0.1', 0))
        return [probe.getsockname()[1] for probe in probes]
    finally:
        for probe in probes:
            probe.close()


def read_record(store):
    path = store/'log/efsd.exit'
    if not path.exists():
        return None
    lines = path.read_text().splitlines()
    assert lines[0] == 'efsd-exit 1', lines
    return dict(line.split('=', 1) for line in lines[1:])


class Daemon:
    def __init__(self, store, port, record_park=None, stderr_fd=None):
        self.store, self.port = store, port
        (store/'log').mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, EFS_FAULT_SHUTDOWN_TOTAL_MS=str(TOTAL_MS),
                   EFS_FAULT_SHUTDOWN_PARK='packer')
        env.pop('EFS_FAULT_SHUTDOWN_RECORD_PARK', None)
        if record_park:
            env['EFS_FAULT_SHUTDOWN_RECORD_PARK'] = record_park
        self.log = (store/'log/efsd.log').open('a')
        self.proc = subprocess.Popen(
            [str(binary), '--cluster', str(self.conf()), '--addr', '127.0.0.1', '--port', str(self.port),
             '--storage', str(store), '--direct-io'],
            cwd=root, env=env, stdout=self.log,
            stderr=stderr_fd if stderr_fd is not None else subprocess.STDOUT)
        self.pidfile = store/'log/efsd.pid'
        self.pidfile.write_text(f'{self.proc.pid}\n')
        deadline = time.monotonic() + 60
        while f'listening on 127.0.0.1:{self.port}' not in self.tail():
            assert self.proc.poll() is None, f'efsd exited during start: {self.tail()}'
            assert time.monotonic() < deadline, f'efsd did not start serving: {self.tail()}'
            time.sleep(0.2)
        with socket.create_connection(('127.0.0.1', self.port), 2):
            pass
        subprocess.run([sys.executable, str(helper), 'record', str(self.pidfile), str(store)],
                       check=True, timeout=10)

    def conf(self):
        """D39: one member of a three-member configured list (the others
        never start). The list belongs to the store: a restart reuses it."""
        path = self.store.parent/(self.store.name + '.cluster.conf')
        path.write_text(f'1 127.0.0.1:{self.port}\n2 127.0.0.2:{self.port}\n3 127.0.0.3:{self.port}\n')
        return path

    def tail(self):
        if not self.log.closed:
            self.log.flush()
        return (self.store/'log/efsd.log').read_text()[-3000:]

    def stop(self):
        """stop-file as stop.sh runs it: (script rc, its output, wall ms)."""
        t0 = time.monotonic()
        out = subprocess.run([sys.executable, str(helper), 'stop-file', str(self.pidfile),
                              str(self.store)], capture_output=True, text=True, timeout=90)
        wall_ms = (time.monotonic() - t0) * 1000
        text = out.stdout + out.stderr
        print(text.strip(), flush=True)
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.kill()
            raise AssertionError(f'efsd PID {self.proc.pid} did not exit within the stop '
                                 f'script wait ({wall_ms:.0f} ms); killed: {text.strip()}')
        finally:
            self.log.close()
        return out.returncode, text, wall_ms

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)


def record_parked_case(step, port):
    store = work/f'record-{step}-parked'
    daemon = Daemon(store, port, record_park=step)
    try:
        rc, out, wall_ms = daemon.stop()
    finally:
        daemon.kill()
    assert daemon.proc.returncode == 3, (daemon.proc.returncode, daemon.tail())
    assert wall_ms <= BOUND_MS, f'stop took {wall_ms:.0f} ms, bound {BOUND_MS} ms'
    assert read_record(store) is None, read_record(store)
    assert rc == 4 and 'outcome unknown (no exit record from this process)' in out, (rc, out)
    assert 'forced' not in out and 'orderly' not in out.replace('not an orderly stop', ''), out
    log = daemon.tail()
    assert f'deadline {TOTAL_MS} ms passed in phase packer' in log, log
    print(f'record-{step}-parked: efsd rc 3, stop wall {wall_ms:.0f} ms (bound {BOUND_MS} ms), '
          'no record, stop script "unknown" rc 4 PASS', flush=True)


def stderr_blocked_case(port):
    store = work/'stderr-blocked'
    rfd, wfd = os.pipe()
    try:
        daemon = Daemon(store, port, stderr_fd=wfd)
        # Fill what the daemon left of the pipe; nobody ever reads it. Fill
        # through a second open file description: O_NONBLOCK on the shared
        # one would make the daemon's own writes fail instead of block. A
        # write up to PIPE_BUF is all or nothing, so finish byte by byte.
        fill = os.open(f'/proc/self/fd/{wfd}', os.O_WRONLY | os.O_NONBLOCK)
        filled = 0
        for chunk in (b'x' * 4096, b'x'):
            try:
                while True:
                    filled += os.write(fill, chunk)
            except BlockingIOError:
                pass
        os.close(fill)
        assert os.get_blocking(wfd), "the daemon's stderr must stay blocking"
        os.close(wfd)
        wfd = -1
        try:
            rc, out, wall_ms = daemon.stop()
        finally:
            daemon.kill()
    finally:
        if wfd >= 0:
            os.close(wfd)
        os.close(rfd)
    assert daemon.proc.returncode == 3, daemon.proc.returncode
    assert wall_ms <= BOUND_MS, f'stop took {wall_ms:.0f} ms, bound {BOUND_MS} ms'
    # The stderr helper really blocked: efsd waited out the report bound.
    assert wall_ms >= TOTAL_MS + REPORT_MS - 500, f'stderr did not block ({wall_ms:.0f} ms)'
    record = read_record(store)
    assert record and record['pid'] == str(daemon.proc.pid), record
    assert record['outcome'] == 'forced' and record['phase'] == 'packer', record
    assert rc == 3 and 'stopped, forced, stores preserved' in out, (rc, out)
    print(f'stderr-blocked: pipe filled with {filled} bytes; efsd rc 3, stop wall '
          f'{wall_ms:.0f} ms (bound {BOUND_MS} ms), record forced, stop script rc 3 PASS',
          flush=True)


cases = {'write': lambda port: record_parked_case('write', port),
         'fsync': lambda port: record_parked_case('fsync', port),
         'stderr': stderr_blocked_case}
# The cases are independent (own store, port, daemon and pipe) and spend their
# time waiting out real deadlines, so they run concurrently: the test takes
# about as long as its longest case instead of the sum. Each keeps its bounds.
names = sys.argv[1:] or list(cases)  # e.g. one case per negative-control run
errors = {}
def run_case(name, port):
    try:
        cases[name](port)
    except BaseException as exc:  # reported after every case has finished
        errors[name] = exc
threads = [threading.Thread(target=run_case, args=(name, port))
           for name, port in zip(names, free_ports(len(names)))]
for t in threads:
    t.start()
for t in threads:
    t.join()
if errors:
    for name, exc in errors.items():
        print(f'{name}: FAIL {exc!r}', flush=True)
    raise next(iter(errors.values()))
shutil.rmtree(work)
print('test_shutdown_blocked_io: OK')
