#!/usr/bin/env python3
"""W125 P0 capture: closed-loop per-file service times of the tiny_files.py workload.

Replicates tiny_files.py's create_folder (folder depth draw, files named
file_NNNNNN.txt, sizes uniform 1..max-file-size from 512-byte base64 blocks,
open('xb') / write / close, one worker per folder at a time, 2*workers folder
tasks queued) and records, per file:

  worker, folder_index, depth, size, t_open, open_us, write_us, fsync_us,
  close_us

t_open is CLOCK_MONOTONIC in microseconds. With --fsync N the worker calls
os.fsync before close on every Nth file (0 = never, as tiny_files.py does).
The output CSV feeds w125_p0_model.py. Raw CSVs belong outside Git.
"""
import argparse, base64, csv, os, random, threading, time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path

def now_us():
    return time.monotonic_ns() // 1000

def create_folder(a, folder_index, count, blocks, rows, lock, worker_of):
    rng = random.Random(time.monotonic_ns() ^ folder_index)
    folder = a.directory
    depth = 1 if folder_index else 0
    if depth:
        while depth < a.max_depth and rng.getrandbits(1):
            depth += 1
    if depth:
        folder /= f"folder_{folder_index:06d}"
        for level in range(2, depth + 1):
            folder /= f"level_{level}"
        folder.mkdir(parents=True)
    worker = worker_of()
    out = []
    for index in range(count):
        block = blocks[rng.getrandbits(10)]
        size = rng.randint(1, a.max_file_size)
        full, rem = divmod(size, len(block))
        path = folder / f"file_{index:06d}.txt"
        t0 = now_us()
        fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        t1 = now_us()
        for _ in range(full):
            os.write(fd, block)
        if rem:
            os.write(fd, block[:rem])
        t2 = now_us()
        if a.fsync and (index + 1) % a.fsync == 0:
            os.fsync(fd)
        t3 = now_us()
        os.close(fd)
        t4 = now_us()
        out.append((worker, folder_index, depth, size, t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3))
    with lock:
        rows.extend(out)
    return count

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path)
    p.add_argument("--max-depth", type=int, default=8)
    p.add_argument("--max-files-per-folder", type=int, default=1000)
    p.add_argument("--max-files", type=int, required=True)
    p.add_argument("--max-file-size", type=int, default=512)
    p.add_argument("--workers", type=int, default=16)
    p.add_argument("--fsync", type=int, default=0, help="fsync every Nth file per folder (0 = never)")
    p.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    a.directory.mkdir(parents=True)
    blocks = tuple(base64.b64encode(os.urandom(384)) for _ in range(1024))
    rows, lock = [], threading.Lock()
    ids, ids_lock = {}, threading.Lock()
    def worker_of():
        t = threading.get_ident()
        with ids_lock:
            return ids.setdefault(t, len(ids))
    rng = random.Random(time.monotonic_ns())
    started = time.monotonic()
    scheduled = folder_count = 0
    with ThreadPoolExecutor(max_workers=a.workers) as ex:
        pending = set()
        while scheduled < a.max_files:
            count = min(rng.randint(1, a.max_files_per_folder), a.max_files - scheduled)
            pending.add(ex.submit(create_folder, a, folder_count, count, blocks, rows, lock, worker_of))
            scheduled += count
            folder_count += 1
            if len(pending) >= a.workers * 2:
                done, pending = wait(pending, return_when=FIRST_COMPLETED)
                for f in done:
                    f.result()
        for f in pending:
            f.result()
    elapsed = time.monotonic() - started
    with a.out.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["worker", "folder", "depth", "size", "t_open_us", "open_us", "write_us", "fsync_us", "close_us"])
        w.writerows(sorted(rows, key=lambda r: r[4]))
    print(f"files={len(rows)} folders={folder_count} workers={a.workers} fsync_every={a.fsync} "
          f"elapsed={elapsed:.1f}s rate={len(rows)/elapsed:.0f} files/s")

if __name__ == "__main__":
    main()
