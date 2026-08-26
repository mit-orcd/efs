#!/usr/bin/env python3
# Repro for basic_chunk_boundary parallel flake: many threads doing a
# straddle-chunk write + same-fd pread, to drive dcache flush/reclaim
# pressure and catch a zero-read.
import os, sys, threading, errno

MNT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs/mnt"
CHUNK = 128 * 1024
THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
ITERS = int(sys.argv[3]) if len(sys.argv) > 3 else 60

fails = []
lock = threading.Lock()

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

ths = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
for th in ths: th.start()
for th in ths: th.join()
print("done: %d fails / %d ops" % (len(fails), THREADS * ITERS), flush=True)
sys.exit(1 if fails else 0)
