#!/usr/bin/env python3
"""K1/W145 R4: the whole efsd stop is bounded, and a forced stop is reported
as forced, never as an orderly one.

Runs tests/shutdown_faults/efsd (efsd.c and packer.c built with EFS_FAULTS;
`make tests/shutdown_faults/efsd`) as a private single-node daemon on a
disposable root, with the total shutdown deadline shortened to TOTAL_MS by
EFS_FAULT_SHUTDOWN_TOTAL_MS, and stops it through
scripts/server_processes.py stop-file exactly as stop.sh does:

1. packer parked (EFS_FAULT_SHUTDOWN_PARK=packer): the handler drain ends at
   once, the packer phase never returns; efsd exits rc 3 within the total
   deadline naming phase packer and the packer thread, with no durability
   barrier reached. The stop script exits 3 and prints
   "stopped, forced, stores preserved". No store file is removed, and an
   unparked restart on the same root starts and serves. Like any unclean
   stop, the forced one leaves the root recovery-uncertain (W71: COMMIT
   EIO until verified recovery or fresh format), so that restart's own
   stop cannot complete the PUT-sync barrier: it must report rc 4,
   "durability barrier failed", never orderly.
2. connection parked (EFS_FAULT_SHUTDOWN_PARK=conn) with one open client
   connection: its handler never finishes; efsd exits rc 3 at the drain's
   share of the deadline naming phase handler-drain.
3. no fault: rc 0, "stopped, orderly", every barrier done.
"""
from pathlib import Path
import os, shutil, socket, subprocess, sys, tempfile, threading, time

root = Path(__file__).resolve().parents[1]
binary = root/'tests/shutdown_faults/efsd'
helper = root/'scripts/server_processes.py'
TOTAL_MS = 6000
SLACK_MS = 2500  # process exit, pidfd wake and Python start-up

if not binary.exists():
    sys.exit(f'{binary} missing: run `make tests/shutdown_faults/efsd` first')

storage_tmp = os.environ.get('EFS_TEST_STORAGE_TMPDIR') or None
work = Path(tempfile.mkdtemp(prefix='efs-shutdown-stuck-', dir=storage_tmp))
print('work:', work, flush=True)


_ports_lock = threading.Lock()
_ports_given = set()


def free_port():
    """A free port never handed out before in this process: the case groups
    run concurrently, and a just-closed probe port can be returned again."""
    with _ports_lock:
        while True:
            with socket.socket() as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            if port not in _ports_given:
                _ports_given.add(port)
                return port


def transient(name):
    # Atomic-rename temporaries that live threads replace until _exit.
    return name.endswith('.new') or '.tmp' in name


def store_files(store):
    return {str(p.relative_to(store)) for p in store.rglob('*')
            if p.is_file() and p.relative_to(store).parts[0] != 'log' and not transient(p.name)}


def read_record(store):
    lines = (store/'log/efsd.exit').read_text().splitlines()
    assert lines[0] == 'efsd-exit 1', lines
    return dict(line.split('=', 1) for line in lines[1:])


class Daemon:
    def __init__(self, store, park=None, port=None):
        # K33: a store keeps its endpoint; a restart reuses the port.
        self.store, self.port = store, port or free_port()
        (store/'log').mkdir(parents=True, exist_ok=True)
        env = dict(os.environ, EFS_FAULT_SHUTDOWN_TOTAL_MS=str(TOTAL_MS))
        env.pop('EFS_FAULT_SHUTDOWN_PARK', None)
        if park:
            env['EFS_FAULT_SHUTDOWN_PARK'] = park
        self.log = (store/'log/efsd.log').open('a')
        self.offset = self.log.tell()  # a restart on the same port appends
        self.proc = subprocess.Popen(
            [str(binary), f'127.0.0.1:{self.port}', str(store), '--discovery-port', '0',
             '--direct-io'],
            cwd=root, env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self.pidfile = store/'log/efsd.pid'
        self.pidfile.write_text(f'{self.proc.pid}\n')
        # The port accepts once bound, before SIGTERM is handled; the banner
        # is printed after the handler is installed, just before serving.
        deadline = time.monotonic() + 60
        while f'listening on 127.0.0.1:{self.port}' not in self.run_log():
            assert self.proc.poll() is None, f'efsd exited during start: {self.tail()}'
            assert time.monotonic() < deadline, f'efsd did not start serving: {self.tail()}'
            time.sleep(0.2)
        with socket.create_connection(('127.0.0.1', self.port), 2):
            pass
        subprocess.run([sys.executable, str(helper), 'record', str(self.pidfile), str(store)],
                       check=True, timeout=10)

    def run_log(self):
        self.log.flush()
        return (self.store/'log/efsd.log').read_text()[self.offset:]

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
        self.proc.wait(10)
        self.log.close()
        print(out.stdout.strip(), flush=True)
        assert not self.pidfile.exists()
        return out.returncode, out.stdout + out.stderr, wall_ms

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)


def forced_case(name, park, phase, worker_text, max_elapsed_ms, hold_connection=False):
    store = work/name
    daemon = Daemon(store, park)
    conn = None
    try:
        if hold_connection:
            conn = socket.create_connection(('127.0.0.1', daemon.port), 2)
            time.sleep(0.5)  # accepted and registered before the stop
        before = store_files(store)
        rc, out, wall_ms = daemon.stop()
    finally:
        if conn:
            conn.close()
        daemon.kill()
    record = read_record(store)
    assert daemon.proc.returncode == 3, (daemon.proc.returncode, daemon.tail())
    assert rc == 3, (rc, out)
    assert 'stopped, forced, stores preserved' in out and f'phase {phase}' in out, out
    assert 'orderly' not in out.replace('not an orderly stop', ''), out
    assert record['pid'] == str(daemon.proc.pid) and record['outcome'] == 'forced', record
    assert record['rc'] == '3' and record['phase'] == phase, record
    assert worker_text in record['worker'] and record['stores'] == 'preserved', record
    assert record['deadline_ms'] == str(TOTAL_MS), record
    for barrier in ('put_sync', 'reclaim_journal', 'usage_flush'):
        assert record[barrier] == 'not_reached', record
    elapsed = int(record['elapsed_ms'])
    assert elapsed <= max_elapsed_ms, (elapsed, max_elapsed_ms)
    assert wall_ms <= TOTAL_MS + SLACK_MS, (wall_ms, TOTAL_MS)
    log = daemon.tail()
    assert f'deadline {TOTAL_MS} ms passed in phase {phase}' in log, log
    missing = before - store_files(store)
    assert not missing, f'forced stop removed store files: {sorted(missing)[:10]}'
    print(f'{name}: forced rc 3 at {elapsed} ms (deadline {TOTAL_MS} ms, stop wall '
          f'{wall_ms:.0f} ms), phase {phase}, worker "{record["worker"]}", '
          f'{len(before)} store files kept PASS', flush=True)
    return store, daemon.port


def after_forced_case(name, store, port):
    files = store_files(store)
    daemon = Daemon(store, port=port)
    try:
        rc, out, wall_ms = daemon.stop()
    finally:
        daemon.kill()
    record = read_record(store)
    log = daemon.tail()
    assert 'recovery uncertain; COMMIT returns EIO' in log, log
    assert daemon.proc.returncode == 4 and rc == 4, (daemon.proc.returncode, rc, out)
    assert 'durability barrier failed, stores preserved' in out and 'put_sync failed' in out, out
    assert record['outcome'] == 'barrier-failed' and record['put_sync'] == 'failed', record
    assert record['reclaim_journal'] == 'done' and record['usage_flush'] == 'done', record
    assert not files - store_files(store), 'restart removed store files'
    print(f'{name}: restarted on the preserved root; its stop reports rc 4 put_sync failed '
          '(W71 recovery-uncertain fence), not orderly PASS', flush=True)


def orderly_case(name, store):
    daemon = Daemon(store)
    try:
        rc, out, wall_ms = daemon.stop()
    finally:
        daemon.kill()
    record = read_record(store)
    assert daemon.proc.returncode == 0, (daemon.proc.returncode, daemon.tail())
    assert rc == 0 and 'stopped, orderly (rc 0' in out, (rc, out)
    assert record['outcome'] == 'orderly' and record['rc'] == '0', record
    assert record['stores'] == 'closed' and record['phase'] == 'done', record
    for barrier in ('put_sync', 'reclaim_journal', 'usage_flush'):
        assert record[barrier] == 'done', record
    assert wall_ms <= TOTAL_MS + SLACK_MS, wall_ms
    print(f'{name}: orderly rc 0 in {int(record["elapsed_ms"])} ms, every barrier done PASS',
          flush=True)


# The drain gets four fifths of the total; teardown phases get the rest.
# Three independent groups (own stores, ports and daemons) run concurrently;
# they mostly wait out real deadlines, and each keeps its bounds. The restart
# reuses the packer case's store, so those two stay in order.
def packer_then_restart():
    packer_store, port = forced_case('packer-parked', 'packer', 'packer', 'packer thread',
                                     TOTAL_MS + 200)
    after_forced_case('restart-after-forced', packer_store, port)


groups = {
    'packer': packer_then_restart,
    'connection': lambda: forced_case('connection-parked', 'conn', 'handler-drain',
                                      'connection handlers', TOTAL_MS * 4 // 5 + 400,
                                      hold_connection=True),
    'orderly': lambda: orderly_case('orderly', work/'orderly'),
}
errors = {}


def run_group(name):
    try:
        groups[name]()
    except BaseException as exc:  # reported after every group has finished
        errors[name] = exc


threads = [threading.Thread(target=run_group, args=(name,)) for name in groups]
for t in threads:
    t.start()
for t in threads:
    t.join()
if errors:
    for name, exc in errors.items():
        print(f'{name}: FAIL {exc!r}', flush=True)
    raise next(iter(errors.values()))
shutil.rmtree(work)
print('test_shutdown_stuck_worker: OK')
