#!/usr/bin/env python3
"""AB/AE shared measurement kit using the actual D4 working-set writer.

Fresh private three-process TCP cluster only. Rate limiting wraps D4's stop
predicate; writer_fixed, its operation mix, payloads and final-version oracle
are imported unchanged. This measures D4's close contract, not per-write fsync.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from types import SimpleNamespace

from pump_logging_profile import ROOT, SinkCluster, cpu, pct, k
sys.path.insert(0, str(ROOT / 'scripts'))
from d4_progress import Run


class PacedStop:
    """Each writer owns a schedule. No catch-up burst after a slow operation."""
    def __init__(self, start, seconds, rate):
        self.start, self.end, self.period = start, start + seconds, 1 / rate
        self.local = threading.local()

    def is_set(self):
        deadline = getattr(self.local, 'next', self.start)
        now = time.monotonic()
        if deadline > now:
            time.sleep(max(0, min(deadline, self.end) - now))
        now = time.monotonic()
        if now >= self.end:
            return True
        self.local.next = max(deadline + self.period, now + self.period)
        return False


def run(a):
    assert a.seconds > 0 and a.rate > 0 and a.threads > 0
    a.output.mkdir(parents=True, exist_ok=False)
    a.work.mkdir(parents=True, exist_ok=False)
    k.EFSD = ROOT / 'tests/faults/efsd'
    env = {key: value for key, value in os.environ.items() if not key.startswith('EFS_')}
    env.update(EFS_TRANSPORT='tcp', EFS_RAFT_OBS=str(a.obs), EFS_BATCH_OBS='1',
               EFS_PACK_MIN_AGE_MS='1000', EFS_PACK_INTERVAL_MS='200',
               EFS_QUERY_REFRESH_MS='2000', EFS_FAULT_MEMTABLE_BYTES='65536')
    c = SinkCluster(a.work, 'cluster', (1, 2, 3), env)
    mount = None
    result = dict(verdict='FAIL', workload='d4_progress.Run.writer_fixed',
                  threads=a.threads, rate_per_writer=a.rate, duration_s=a.seconds,
                  obs=a.obs, working_set_per_writer=16, errors=[])
    try:
        for i in (1, 2, 3):
            c.start(i, None if i == 1 else 1)
        c.wait_members((1, 2, 3), 3)
        c.wait_voters((1, 2, 3), lambda n, gs: set(gs) == {0, 2} and
                      all(hosted and voters == 7 for hosted, voters in gs.values()))
        mount = k.Mount(c, 1, 'mnt')
        load = Run(SimpleNamespace(mount=mount.path, fg_mount=mount.path,
                                   out=a.output, working_set=16), None)
        load.dir.mkdir()
        for w in range(a.threads):
            load.populate(w)
        assert not load.errors, load.errors
        probe = load.dir / 'probe'
        probe.write_bytes(b'probe')
        def status(label):
            for i in (1, 2, 3):
                rc, info = k.mgmt(env, 'raft-status', c.ep(i))
                (a.output / ('raft-%s-n%d.txt' % (label, i))).write_text(info)
                assert rc == 0, (label, i, rc)
        status('before')
        start = time.monotonic()
        end = start + a.seconds
        load.stop_load = PacedStop(start, a.seconds, a.rate)
        stats, thread_errors = [], []
        def writer(w):
            try:
                load.writer_fixed(w)
            except BaseException as e:
                thread_errors.append(repr(e))
        def statter():
            deadline = start
            while time.monotonic() < end:
                t0 = time.monotonic()
                try:
                    os.stat(probe)
                    stats.append(time.monotonic() - t0)
                except OSError as e:
                    thread_errors.append(repr(e))
                deadline += .1
                time.sleep(max(0, min(deadline, end) - time.monotonic()))
        before_cpu = {i: cpu(p.pid) for i, p in c.procs.items()}
        threads = [threading.Thread(target=writer, args=(w,)) for w in range(a.threads)]
        threads.append(threading.Thread(target=statter))
        for t in threads:
            t.start()
        time.sleep(max(0, end - time.monotonic()))
        cpu_window = {i: cpu(p.pid) - before_cpu[i] for i, p in c.procs.items()}
        for t in threads:
            t.join(90)
            assert not t.is_alive(), 'D4 worker did not finish'
        # Never accept any historical version: require each final acknowledged
        # D4 version after writers stop, and keep every write/read failure.
        errors = load.errors + thread_errors
        for name, size in load.live.items():
            try:
                if name.read_bytes() != load.content(str(name.relative_to(load.dir)), name, size):
                    errors.append('final-version mismatch: %s' % name)
            except OSError as e:
                errors.append('final read %s: %r' % (name, e))
        time.sleep(6)  # reporter cadence, outside application/CPU window
        status('after')
        writes = sum(load.counts[key] for key in ('overwritten', 'truncated', 'rewritten'))
        result.update(verdict='PASS' if not errors else 'FAIL', errors=errors,
                      counts=load.counts, writes=writes, write_ops_s=writes/a.seconds,
                      daemon_cpu_s=cpu_window, stat_count=len(stats),
                      stat_p50_s=pct(stats, .5), stat_p99_s=pct(stats, .99),
                      verified_files=len(load.live), uncertain_files=len(load.uncertain))
        assert not errors and not load.uncertain, result
        assert writes >= .9 * a.seconds * a.threads * a.rate, 'offered rate not sustained'
        return result
    except BaseException as e:
        result.update(verdict='FAIL', error=repr(e))
        raise
    finally:
        try:
            if mount:
                stop = subprocess.run([str(ROOT/'scripts/client.sh'), 'stop', str(mount.path)],
                                        env=env, text=True, capture_output=True, timeout=650)
                (a.output/'mount-stop.txt').write_text(stop.stdout + stop.stderr)
                assert stop.returncode == 0 and not os.path.ismount(mount.path)
                assert mount.p.wait(timeout=30) == 0
                mount.log.close()
            for i, proc in c.procs.items():
                proc.send_signal(signal.SIGTERM)
                assert proc.wait(timeout=60) == 0, ('daemon stop', i)
        except BaseException as e:
            result.update(verdict='FAIL', cleanup_error=repr(e))
            raise
        finally:
            for p in c.dir.glob('*.log'):
                (a.output/p.name).write_bytes(p.read_bytes())
            (a.output/'result.json').write_text(json.dumps(result, indent=2))


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--obs', type=int, choices=(0, 1), required=True)
    p.add_argument('--seconds', type=float, default=30)
    p.add_argument('--threads', type=int, default=4)
    p.add_argument('--rate', type=float, default=5)
    print(json.dumps(run(p.parse_args())))
