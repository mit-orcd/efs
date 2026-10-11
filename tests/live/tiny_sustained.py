"""Bounded sustained CREATE/name-reuse churn with cold-verifiable survivors."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import threading
import time


def payload(worker, generation):
    return hashlib.sha256(f'{worker}:{generation}'.encode()).digest() * 16


def quantiles(values):
    values = sorted(values)
    return {f'p{q}_us': values[min(len(values)-1, (len(values)*q+99)//100-1)]
            for q in (50, 95, 99)} | {'max_us': values[-1]}


def exercise(mount, pids, seconds, evidence, large_mib_s=0):
    assert 1 <= seconds <= 1800 and 0 <= large_mib_s <= 64
    folder = Path(mount) / 'tiny-sustained'
    folder.mkdir()
    tiny_workers, large_workers, slots = 8, 4, 64
    gate = threading.Barrier(tiny_workers + large_workers)
    stop = threading.Event()
    samples = {}
    lock = threading.Lock()
    start = time.monotonic()
    deadline = start + seconds
    process_samples = []
    def measure(kind, duration_us):
        window = int((time.monotonic()-start)//30)
        with lock:
            sample = samples.setdefault(window, {'open': [], 'close': [], 'delete': []})
            sample[kind].append(duration_us)
    def tiny(worker):
        gate.wait(timeout=30)
        generation = 0
        while True:
            generation += 1
            data = payload(worker, generation)
            for slot in range(slots):
                path = folder / f'tiny-{worker}-{slot}'
                before = time.monotonic_ns()
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                measure('open', (time.monotonic_ns()-before)/1000)
                try:
                    assert os.write(fd, data) == len(data)
                finally:
                    before = time.monotonic_ns(); os.close(fd)
                    measure('close', (time.monotonic_ns()-before)/1000)
            # Every acknowledged generation is byte-checked before retirement.
            for slot in range(slots):
                path = folder / f'tiny-{worker}-{slot}'
                assert path.read_bytes() == data, path
            if stop.is_set() or time.monotonic() >= deadline:
                return generation
            for slot in range(slots):
                before = time.monotonic_ns()
                (folder / f'tiny-{worker}-{slot}').unlink()
                measure('delete', (time.monotonic_ns()-before)/1000)
    def large(worker):
        data = hashlib.sha256(f'large-{worker}'.encode()).digest()*32768
        fd = os.open(folder/f'large-{worker}', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        written = 0
        gate.wait(timeout=30)
        writer_start = time.monotonic()
        try:
            while written < 16 or not stop.is_set():
                assert os.pwrite(fd, data, (written % 16)*len(data)) == len(data)
                written += 1
                if written % 4 == 0: os.fsync(fd)
                if large_mib_s:
                    delay = written/large_mib_s-(time.monotonic()-writer_start)
                    if delay > 0: stop.wait(delay)
            os.fsync(fd)
        finally: os.close(fd)
        return written*len(data)
    def process_snapshot():
        result = {}
        for pid in pids:
            status = Path(f'/proc/{pid}/status').read_text()
            rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith('VmRSS:'))
            fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
            result[str(pid)] = {'rss_kib': rss, 'cpu_ticks': int(fields[11])+int(fields[12])}
        return result
    futures = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        big = [pool.submit(large, i) for i in range(large_workers)]
        small = [pool.submit(tiny, i) for i in range(tiny_workers)]
        futures = big + small
        try:
            while time.monotonic() < deadline:
                for future in futures:
                    if future.done() and future.exception(): raise future.exception()
                process_samples.append({'seconds': time.monotonic()-start, 'processes': process_snapshot()})
                with lock:
                    counts = {str(k): {n: len(v) for n,v in s.items()} for k,s in samples.items()}
                record = {'elapsed_s': time.monotonic()-start, 'window_counts': counts,
                          'process_samples': process_samples}
                Path(evidence).write_text(json.dumps(record, indent=2)+'\n')
                print(f'sustained seconds={record["elapsed_s"]:.1f} counts={counts}', flush=True)
                stop.wait(min(30, max(0, deadline-time.monotonic())))
            generations = [f.result(timeout=120) for f in small]
        finally: stop.set()
        written = [f.result(timeout=120) for f in big]
    windows = []
    for window, sample in sorted(samples.items()):
        windows.append({'window': window, 'counts': {k:len(v) for k,v in sample.items()},
                        **{k: quantiles(v) for k,v in sample.items() if v}})
    record = {'requested_seconds': seconds, 'wall_s': time.monotonic()-start,
              'tiny_workers': tiny_workers, 'slots_per_worker': slots, 'tiny_bytes': 512,
              'generations': generations, 'created': sum(generations)*slots,
              'warm_verified': sum(generations)*slots, 'retained': tiny_workers*slots,
              'large_workers': large_workers, 'large_live_bytes_each':16<<20,
              'large_offered_mib_s_per_worker':large_mib_s,
              'large_written_bytes': written, 'windows': windows,
              'process_samples':process_samples, 'cpu_hz':os.sysconf('SC_CLK_TCK')}
    assert record['wall_s'] >= seconds
    Path(evidence).write_text(json.dumps(record, indent=2)+'\n')
    return record


def verify_and_delete(mount, generations):
    folder = Path(mount)/'tiny-sustained'
    for worker,generation in enumerate(generations):
        for slot in range(64):
            path = folder/f'tiny-{worker}-{slot}'
            assert path.read_bytes() == payload(worker,generation), path
            path.unlink()
    for worker in range(4):
        path = folder/f'large-{worker}'
        data = hashlib.sha256(f'large-{worker}'.encode()).digest()*32768
        with path.open('rb') as stream:
            for _ in range(16): assert stream.read(len(data)) == data, path
            assert stream.read(1) == b'', path
        path.unlink()
    folder.rmdir()
