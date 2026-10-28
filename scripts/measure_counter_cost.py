#!/usr/bin/env python3
"""Task AD: one owned, private three-daemon D4 counter-cost measurement.

Accepts an already-built source tree (normal or an external local experiment).
Never disables counters, changes product code, or profiles unrelated processes.
Run cases serially: profiler overhead is part of these diagnostic timings.
Raw results and scratch must be outside Git; scratch is retained for checked
cleanup and evidence extraction. See ad-counter-contention-20261028.md.
"""
import argparse
import json
import os
from pathlib import Path
import random
import signal
import socket
import subprocess
import sys
import threading
import time
import traceback


def save(path, value):
    path.write_text(json.dumps(value, indent=2, default=str) + '\n')


def cpu(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True)
    p.add_argument('--scratch', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--writers', type=int, choices=(8, 16, 32), required=True)
    p.add_argument('--transport', choices=('tcp', 'rdma'), required=True)
    p.add_argument('--host', default='127.0.0.1')
    p.add_argument('--load-s', type=int, default=90)
    p.add_argument('--profile-s', type=int, default=60)
    a = p.parse_args()
    assert a.load_s >= a.profile_s + 15
    a.source = a.source.resolve()
    a.scratch.mkdir(parents=True, exist_ok=False)
    a.out.mkdir(parents=True, exist_ok=False)
    sys.path[:0] = [str(a.source/'tests'), str(a.source/'scripts')]
    import k33_daemons as k
    import d4_progress as d4

    # Private reserved range, never the deployment or other tasks' 17xxx ports.
    def ports(n):
        result = []
        for port in random.sample(range(19000, 20000), 1000):
            with socket.socket() as sock:
                try:
                    sock.bind((a.host, port))
                except OSError:
                    continue
                result.append(port)
            if len(result) == n:
                return sorted(result)
        raise RuntimeError('private port range exhausted')
    k.free_ports = ports
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('EFS_', 'TMPDIR'))}
    env.update(EFS_TRANSPORT=a.transport, EFS_RDMA_DEV='mlx5_0', EFS_RAFT_OBS='1',
               EFS_PACK_MIN_AGE_MS='1000', EFS_PACK_INTERVAL_MS='200',
               EFS_QUERY_REFRESH_MS='2000', EFS_FAULT_MEMTABLE_BYTES='262144',
               TMPDIR=str(a.scratch))
    c = k.Cluster(a.scratch, 'cluster', [1, 2, 3], env,
                  hosts={i: a.host for i in (1, 2, 3)})
    mounts, profilers, handles = [], [], []
    result = {'arguments': vars(a),
              'environment': {key: value for key, value in env.items()
                              if key.startswith('EFS_') or key == 'TMPDIR'},
              'cleanup': [], 'errors': [],
              'profile_commands': []}
    profiling = threading.Event()
    finished = threading.Event()
    latencies, stat_errors = [], []

    def stat_loop():
        # FUSE syscall latency, not claimed as one uncached RPC per call.
        while not finished.is_set():
            begin = time.monotonic()
            try:
                os.stat(mounts[1].path)
                latencies.append(time.monotonic()-begin)
            except OSError as error:
                stat_errors.append(str(error))
            finished.wait(max(0, .1-(time.monotonic()-begin)))

    def profile():
        try:
            if not profiling.wait(120):
                raise RuntimeError('D4 load did not start')
            time.sleep(10)  # exclude formation/initialization and warm the load
            ids = ','.join(str(proc.pid) for proc in c.procs.values())
            result['profile_start_epoch_s'] = time.time()
            before = {i: cpu(proc.pid) for i, proc in c.procs.items()}
            for i, proc in c.procs.items():
                (a.out/f'n{i}.maps').write_text(Path(f'/proc/{proc.pid}/maps').read_text())
                cmd = ['perf', 'stat', '-x', ';', '-e',
                       'task-clock,cycles,instructions,cache-references,cache-misses',
                       '-p', str(proc.pid), '-o', str(a.out/f'n{i}-stat.csv'),
                       '--', 'sleep', str(a.profile_s)]
                result['profile_commands'].append(cmd)
                profilers.append(subprocess.Popen(cmd, env=env))
            for name, event in [('cycles', ['-e', 'cycles', '-F', '499', '-g', '--call-graph', 'fp']),
                                ('memory', ['-e', 'ibs_op//', '-c', '1048576', '-d'])]:
                log = open(a.out/f'{name}-record.log', 'w'); handles.append(log)
                cmd = ['perf', 'record', '-v', '-N', '-m', '64', *event, '-p', ids, '-o',
                       str(a.out/f'{name}.data'), '--', 'sleep', str(a.profile_s)]
                result['profile_commands'].append(cmd)
                profilers.append(subprocess.Popen(cmd, stdout=log, stderr=log, env=env))
            result['profiler_rc'] = [proc.wait(timeout=a.profile_s+30) for proc in profilers]
            result['profile_cpu_s'] = {i: cpu(proc.pid)-before[i] for i, proc in c.procs.items()}
            result['profile_end_epoch_s'] = time.time()
        except Exception:
            result['errors'].append(traceback.format_exc())

    threads = []
    try:
        for i in (1, 2, 3):
            c.start(i, join=None if i == 1 else 1, quota='8G',
                    args=('--writers', '32'))  # O_DIRECT is unconditional.
        for i in (1, 2, 3):
            c.wait_log(i, r'raft-host: formed: 3 members', timeout=60)
        c.wait_members([1, 2, 3], 3)
        result['pids'] = {i: proc.pid for i, proc in c.procs.items()}
        result['endpoints'] = {i: c.ep(i) for i in c.procs}
        result['leaders_before'] = c.leader_placement(1)
        save(a.out/'running.json', result)
        mounts.append(k.Mount(c, 1, 'load'))
        mounts.append(k.Mount(c, 2, 'foreground'))
        # Signal the profiler from the actual first load sample, not mount time.
        original = d4.Run.sample
        def sample(run, phase):
            value = original(run, phase)
            if phase == 'load':
                profiling.set()
            return value
        d4.Run.sample = sample
        threads = [threading.Thread(target=profile), threading.Thread(target=stat_loop)]
        for thread in threads:
            thread.start()
        result['d4_rc'] = d4.main([
            '--mount', str(mounts[0].path), '--fg-mount', str(mounts[1].path),
            '--seed', c.ep(1), '--mgmt', str(k.MGMT), '--out', str(a.out/'d4'),
            '--load-s', str(a.load_s), '--threads', str(a.writers), '--drain-s', '150',
            '--stall-s', '30', '--server-rss-mib', '2048', '--pack-backlog', '50000',
            '--index-age-s', '60', '--keep'])
        result['leaders_after'] = c.leader_placement(1)
        result['io_stats'] = {}
        for i in c.procs:
            reply = subprocess.run([str(k.MGMT), 'io-stats', c.ep(i)],
                                   env=env, capture_output=True, text=True, timeout=30)
            result['io_stats'][i] = {'rc': reply.returncode, 'out': reply.stdout+reply.stderr}
    except Exception:
        result['errors'].append(traceback.format_exc())
    finally:
        finished.set()
        for thread in threads:
            thread.join(timeout=a.profile_s+160)
            if thread.is_alive():
                result['errors'].append('measurement thread did not finish')
        for proc in profilers:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
                proc.wait(timeout=30)
        for handle in handles:
            handle.close()
        save(a.out/'stat-syscalls.json', {'seconds': latencies, 'errors': stat_errors})
        # No lazy unmount, forced discard or removal of a failed fixture.
        for mount in reversed(mounts):
            try:
                stop = subprocess.run([str(a.source/'scripts/client.sh'), 'stop', str(mount.path)],
                                      env=env, capture_output=True, text=True, timeout=180)
                record = {'mount': str(mount.path), 'rc': stop.returncode,
                          'stdout': stop.stdout, 'stderr': stop.stderr,
                          'mounted': os.path.ismount(mount.path)}
                result['cleanup'].append(record)
                assert stop.returncode == 0 and not record['mounted'], record
                assert mount.p.wait(timeout=30) == 0
                mount.log.close()
            except Exception:
                result['errors'].append(traceback.format_exc())
        if all(not os.path.ismount(mount.path) for mount in mounts):
            for i, proc in c.procs.items():
                try:
                    if proc.poll() is None:
                        proc.send_signal(signal.SIGTERM)
                    rc = proc.wait(timeout=90)
                    exit_record = (c.dir/f'n{i}/log/efsd.exit').read_text()
                    result['cleanup'].append({'daemon': i, 'rc': rc, 'exit': exit_record})
                    assert rc == 0 and 'outcome=orderly' in exit_record
                except Exception:
                    result['errors'].append(traceback.format_exc())
        for path in c.dir.glob('*.log'):
            (a.out/path.name).write_bytes(path.read_bytes())
        for held in c.held:
            held.close()
        save(a.out/'measurement.json', result)
    return int(bool(result['errors'] or stat_errors or result.get('d4_rc', 1) or
                    any(result.get('profiler_rc', [1]))))


if __name__ == '__main__':
    sys.exit(main())
