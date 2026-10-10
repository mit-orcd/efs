#!/usr/bin/env python3
"""Create tiny files reusing a pool of 1024 random base64 payloads within a maximum folder depth."""

import argparse
import base64
import os
import random
import sys
import threading
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path


class Progress:
    def __init__(self, total):
        self.total = total
        self.created = 0
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.started = time.monotonic()
        self.width = 0
        self.thread = threading.Thread(target=self.report, daemon=True)

    def advance(self, count):
        with self.lock:
            self.created += count

    def display(self):
        with self.lock:
            created = self.created
        elapsed = time.monotonic() - self.started
        percent = 100 * created / self.total if self.total else 100
        line = (f"{created:,}/{self.total:,} files ({percent:.1f}%), "
                f"{elapsed:.1f} s, {created / max(elapsed, 1e-9):,.0f} files/s")
        self.width = max(self.width, len(line))
        print("\r" + line.ljust(self.width), end="", file=sys.stderr, flush=True)

    def report(self):
        while not self.stop.wait(1):
            self.display()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.stop.set()
        self.thread.join()
        self.display()
        print(file=sys.stderr, flush=True)


def create_folder(args, folder_index, count, progress, blocks):
    # Each task owns its RNG; no shared RNG or per-file clock reads.
    rng = random.Random(time.monotonic_ns() ^ folder_index)
    folder = args.directory
    # The first batch uses the base directory (depth zero).
    depth = 1 if folder_index else 0
    # Favor shallow folders: each extra level has a 50% chance.
    if depth:
        while depth < args.max_depth and rng.getrandbits(1):
            depth += 1
    if depth:
        folder /= f"folder_{folder_index:06d}"
        for level in range(2, depth + 1):
            folder /= f"level_{level}"
        folder.mkdir(parents=True)
    pick = rng.getrandbits
    pending = 0
    try:
        for index in range(count):
            block = blocks[pick(10)]
            size = rng.randint(1, args.max_file_size)
            full_blocks, remainder = divmod(size, len(block))
            with (folder / f"file_{index:06d}.txt").open("xb") as handle:
                for _ in range(full_blocks):
                    handle.write(block)
                if remainder:
                    handle.write(block[:remainder])
            pending += 1
            # Keep the shared progress lock out of the per-file hot path.
            if pending == 64:
                progress.advance(pending)
                pending = 0
    finally:
        if pending:
            progress.advance(pending)
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="new base directory (must not exist)")
    parser.add_argument("--max-depth", type=int, default=1, help="maximum folder levels below directory; favors shallow folders; directory itself is depth 0 (default: 1)")
    parser.add_argument("--max-files-per-folder", type=int, default=10, help="random file count per populated directory, from 1 to this maximum (default: 10)")
    parser.add_argument("--max-files", type=int, required=True, help="maximum total number of files to create")
    parser.add_argument("--max-file-size", type=int, default=512, help="random file size from 1 byte to this maximum in bytes (default: 512)")
    parser.add_argument("--workers", type=int, default=4, help="parallel folder-writing threads (default: 4; use 1 for sequential writes)")
    args = parser.parse_args()
    if args.max_depth < 0 or args.max_files < 0:
        parser.error("max-depth and max-files must be nonnegative")
    if args.max_files_per_folder < 1 or args.max_file_size < 1 or args.workers < 1:
        parser.error("max-files-per-folder, max-file-size, and workers must be positive")
    if args.directory.exists():
        parser.error("output directory already exists; choose a new directory")

    # 384 random bytes encode to exactly 512 readable ASCII bytes.
    blocks = tuple(base64.b64encode(os.urandom(384))
                   for _ in range(1024))
    args.directory.mkdir(parents=True)
    created = 0
    target = min(args.max_files, args.max_files_per_folder) if args.max_depth == 0 else args.max_files
    folder_count = 0
    scheduled = 0
    rng = random.Random(time.monotonic_ns())
    # Bound queued tasks so very large totals do not consume excessive memory.
    with Progress(target) as progress, ThreadPoolExecutor(max_workers=args.workers) as executor:
        pending = set()
        while scheduled < target:
            count = min(rng.randint(1, args.max_files_per_folder), target - scheduled)
            if args.max_depth == 0:
                count = target
            pending.add(executor.submit(create_folder, args, folder_count, count, progress, blocks))
            scheduled += count
            folder_count += 1
            if len(pending) >= args.workers * 2:
                done, pending = wait(pending, return_when=FIRST_COMPLETED)
                created += sum(future.result() for future in done)
        for future in pending:
            created += future.result()
    print(f"Created {created} files in {folder_count} populated directories under {args.directory.resolve()}")


if __name__ == "__main__":
    main()
