#!/usr/bin/env python3
"""W166 decision evidence, not an acceptance test or a product policy change.

Run only on the assigned private build host, after building all and
tests/faults/efsd. Mirrors K22's 2 MiB L0 / 1 MiB memtable leader-only
compaction stall. Records separate application operations and one-second
raft-status observations, and keeps all logs/stores for reviewed cleanup.
Exit 1 = close failure reproduced, 2 = not reproduced, 3 = harness failure.
No automatic retry. Optional strace is diagnostic evidence only.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from k33_daemons import Cluster, EFSD, MGMT, FUSE, mgmt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--trace-client', action='store_true')
    args = parser.parse_args()
    work = args.work.resolve()
    assert work.is_relative_to(Path.home()) and not str(work).startswith('/data1/')
    assert 60 <= args.seconds <= 600
    work.mkdir(parents=True, exist_ok=False)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('EFS_', 'K22_'))}
    env.update(EFS_TRANSPORT='tcp', EFS_RPC_PROF='1', EFS_REPORT_DBG='1', EFS_WRITE_PROF='1')
    if args.trace_client:
        env['EFS_PUT_TICKET_DBG'] = '1'  # existing trace switch; no behavior selection
    start = time.monotonic()
    stop = threading.Event()
    lock = threading.Lock()
    events = (work / 'application.jsonl').open('w', buffering=1)
    statuses = (work / 'status.jsonl').open('w', buffering=1)
    confirmed = []
    failures = []
    thread_errors = []
    threads = []
    client = tracer = None
    log_thread = None
    c = Cluster(work, 'cluster', [1, 2, 3], env)
    mount = c.dir / 'mnt'
    marker = work / 'compactor-parked'
    result = dict(method='K22 leader-only compressed-budget pressure', trace_client=args.trace_client,
                  driver_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  l0_bytes=2 << 20, memtable_bytes=1 << 20, seconds=args.seconds,
                  product_changed=False, outcome='HARNESS_FAILURE')

    def record(**row):
        row.update(epoch=time.time(), elapsed=round(time.monotonic() - start, 6))
        with lock:
            events.write(json.dumps(row) + '\n')

    def status(label):
        for i in (1, 2, 3):
            rc, text = mgmt(env, 'raft-status', c.ep(i))
            statuses.write(json.dumps(dict(epoch=time.time(), elapsed=time.monotonic() - start,
                                          label=label, node=i, rc=rc, text=text)) + '\n')

    def writer(number):
        try:
            parent = mount / f'writer-{number}'
            seq = 0
            while not stop.is_set():
                seq += 1
                name = parent / str(seq)
                payload = hashlib.sha256(f'{number}:{seq}'.encode()).digest() * 64
                fd = None
                successful = True
                for stage in ('open', 'write', 'close'):
                    began = time.monotonic()
                    record(writer=number, seq=seq, stage=stage, event='begin', path=str(name))
                    try:
                        if stage == 'open':
                            fd = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                        elif stage == 'write':
                            ino = os.fstat(fd).st_ino
                            assert os.write(fd, payload) == len(payload)
                        else:
                            closing, fd = fd, None  # close failure never authorizes closing the fd again
                            os.close(closing)
                        record(writer=number, seq=seq, stage=stage, event='ok',
                               ms=1000 * (time.monotonic() - began),
                               ino=ino if stage != 'open' else None)
                    except OSError as error:
                        row = dict(writer=number, seq=seq, stage=stage, event='error',
                                   errno=error.errno, message=str(error),
                                   ms=1000 * (time.monotonic() - began), path=str(name))
                        failures.append(row)
                        record(**row)
                        successful = False
                        break
                if fd is not None:
                    try:
                        os.close(fd)
                    except OSError as error:
                        record(writer=number, seq=seq, stage='cleanup-close', event='error', errno=error.errno)
                if successful:
                    confirmed.append((name, payload))
        except Exception as error:
            thread_errors.append(repr(error))
            record(writer=number, stage='thread', event='failure', message=repr(error))

    def bench(number):
        try:
            seq = 0
            while not stop.is_set():
                seq += 1
                with (work / f'bench-{number}.log').open('a') as log:
                    p = subprocess.Popen([str(ROOT / 'efs-bench'), c.ep(1), '--meta', '--files', '2000',
                                          '--dirs', '16', '--workers', '16', '--phases', 'mkdir,create',
                                          '--keep', '--id', str(number * 1000000 + seq)],
                                         stdout=log, stderr=subprocess.STDOUT, env=env)
                    while p.poll() is None and not stop.wait(.25):
                        pass
                    if p.poll() is None:
                        p.terminate()
                        try:
                            p.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            p.kill()
                            p.wait()
        except Exception as error:
            thread_errors.append(repr(error))
            record(bench=number, event='failure', message=repr(error))

    def client_log():
        with (work / 'client.log').open('w', buffering=1) as log:
            for line in client.stdout:
                log.write(f'{time.time():.6f} {line}')

    try:
        c.start(1, extra_env={'EFS_FAULT_L0_BYTES': str(2 << 20),
                            'EFS_FAULT_MEMTABLE_BYTES': str(1 << 20),
                            'EFS_FAULT_COMPACT_STALL': '1', 'EFS_FAULT_COMPACT_FILE': str(marker)})
        for i in (2, 3):
            c.start(i, join=1)
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=120)
        c.wait_voters([1, 2, 3], lambda i, v: v == {0: (1, 7), 2: (1, 7)})
        rc, text = mgmt(env, 'raft-status', c.ep(1))
        assert rc == 0 and 'group 0 hosted=1 role=LEADER' in text, text
        mount.mkdir()
        client = subprocess.Popen([str(FUSE), c.ep(2), str(mount), '-f'], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, env=env)
        log_thread = threading.Thread(target=client_log, daemon=True)
        log_thread.start()
        deadline = time.monotonic() + 30
        while not os.path.ismount(mount) and time.monotonic() < deadline:
            assert client.poll() is None, 'client exited before mount'
            time.sleep(.1)
        assert os.path.ismount(mount), 'mount deadline'
        for i in range(4):
            (mount / f'writer-{i}').mkdir()
        if args.trace_client:
            tracer = subprocess.Popen(['strace', '-ff', '-ttt', '-T', '-yy', '-s', '80',
                                       '-e', 'trace=connect,sendto,recvfrom,poll,ppoll', '-o', str(work / 'client.trace'),
                                       '-p', str(client.pid)], stdout=subprocess.DEVNULL,
                                      stderr=(work / 'strace.log').open('w'))
            time.sleep(1)
            assert tracer.poll() is None, 'strace attach failed'
        status('before-pressure')
        marker.touch()
        record(event='pressure-begin', endpoints=c.ports, client_pid=client.pid)
        threads = [threading.Thread(target=writer, args=(i,), daemon=True) for i in range(4)]
        threads += [threading.Thread(target=bench, args=(i,), daemon=True) for i in range(4)]
        for t in threads:
            t.start()
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            status('pressure')
            if thread_errors:
                raise RuntimeError(thread_errors)
            time.sleep(1)
        marker.unlink()
        record(event='pressure-end')
        stop.set()
        for t in threads:
            t.join(timeout=60)
        assert not any(t.is_alive() for t in threads), 'writer did not drain'
        assert not thread_errors, thread_errors
        status('after-pressure')
        for name, expected in confirmed:
            assert name.read_bytes() == expected, ('successful close readback mismatch', name)
        result.update(outcome='REPRODUCED' if any(r['stage'] == 'close' for r in failures) else 'NOT_REPRODUCED',
                      confirmed_close_files=len(confirmed), failures=failures, verified_files=len(confirmed))
    except Exception as error:
        result['harness_error'] = repr(error)
        record(event='harness-failure', message=repr(error))
    finally:
        marker.unlink(missing_ok=True)
        stop.set()
        for t in threads:
            t.join(timeout=60)
        if tracer is not None:
            tracer.send_signal(signal.SIGINT)
            tracer.wait(timeout=15)
        if client is not None:
            subprocess.run(['fusermount3', '-u', str(mount)], capture_output=True, timeout=30)
            try:
                client.wait(timeout=30)
            except subprocess.TimeoutExpired:
                client.kill()
                client.wait()
        c.stop_all()
        if log_thread:
            log_thread.join(timeout=10)
        result['seconds_total'] = time.monotonic() - start
        (work / 'result.json').write_text(json.dumps(result, indent=2))
        events.close()
        statuses.close()
    print(json.dumps(result), flush=True)
    return {'REPRODUCED': 1, 'NOT_REPRODUCED': 2, 'HARNESS_FAILURE': 3}[result['outcome']]


if __name__ == '__main__':
    raise SystemExit(main())
