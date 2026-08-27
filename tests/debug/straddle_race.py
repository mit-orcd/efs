#!/usr/bin/env python3
# Repro for basic_chunk_boundary parallel flake: many threads doing a
# straddle-chunk write + same-fd pread, while a churn thread writes a large
# file to keep the dcache under reclaim pressure (working set > 2 GiB cap).
import os, sys, threading, errno, time

MNT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs-mount"
CHUNK = 128 * 1024
THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
ITERS = int(sys.argv[3]) if len(sys.argv) > 3 else 60
CHURN_GIB = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0

fails = []
lock = threading.Lock()
stop = False

def worker(t):
    d = os.path.join(MNT, "strace-%d" % t)
    os.makedirs(d, exist_ok=True)
    for it in range(ITERS):
        p = os.path.join(d, "f%d" % it)
        try:
            fd = os.open(p, os.O_CREAT | os.O_RDWR, 0o644)
            try:
                os.write(fd, b"A" * CHUNK)
                os.write(fd, b"B")
                os.lseek(fd, CHUNK - 2, os.SEEK_SET)
                os.write(fd, b"XY")
                got = os.pread(fd, 3, CHUNK - 2)
                if got != b"XYB":
                    with lock:
                        fails.append((t, it, got))
                        print("FAIL t=%d it=%d got=%r" % (t, it, got), flush=True)
            finally:
                os.close(fd)
        except OSError as e:
            with lock:
                fails.append((t, it, ("oserr", e.errno)))
                print("OSERR t=%d it=%d errno=%d" % (t, it, e.errno), flush=True)

def churn():
    # Keep the dcache under reclaim pressure: write a big file in 1 MiB
    # chunks, over and over, so dirty_bytes exceeds the 2 GiB reclaim cap
    # and the reclaimer flushes+evicts hot straddle chunks mid-read.
    p = os.path.join(MNT, "strace-churn")
    blk = b"C" * (1024 * 1024)
    nchunks = int(CHURN_GIB * 1024)
    while not stop:
        try:
            fd = os.open(p, os.O_CREAT | os.O_WRONLY | os.O_TRUNC, 0o644)
            try:
                for _ in range(nchunks):
                    if stop:
                        break
                    os.write(fd, blk)
            finally:
                os.close(fd)
        except OSError:
            pass

ct = threading.Thread(target=churn, daemon=True)
ct.start()
ths = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
for th in ths: th.start()
for th in ths: th.join()
stop = True
ct.join(timeout=5)
try:
    os.unlink(os.path.join(MNT, "strace-churn"))
except OSError:
    pass
print("done: %d fails / %d ops" % (len(fails), THREADS * ITERS), flush=True)
sys.exit(1 if fails else 0)
