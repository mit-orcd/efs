#!/usr/bin/env python3
# Repro for dir_many_files parallel flake: many threads each create N files in
# their own dir, listdir (want N), unlink all, listdir (want 0). On leftover,
# stat each leftover to distinguish real (unlink lost) vs phantom (readdir).
import os, sys, threading

MNT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs-mount"
THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
NFILES = int(sys.argv[3]) if len(sys.argv) > 3 else 300
ITERS = int(sys.argv[4]) if len(sys.argv) > 4 else 40

fails = []
lock = threading.Lock()

def note(*a):
    with lock:
        fails.append(a)
        print("FAIL", *a, flush=True)

def worker(t):
    d = os.path.join(MNT, "dm-%d" % t)
    os.makedirs(d, exist_ok=True)
    for it in range(ITERS):
        sub = os.path.join(d, "s%d" % it)
        os.makedirs(sub, exist_ok=True)
        names = ["f%03d" % i for i in range(NFILES)]
        for nm in names:
            p = os.path.join(sub, nm)
            with open(p, "wb") as f:
                f.write(b"x")
        got = os.listdir(sub)
        if len(got) != NFILES:
            note(t, it, "create-listdir", len(got), NFILES)
        for nm in names:
            os.unlink(os.path.join(sub, nm))
        left = os.listdir(sub)
        if left:
            real = []
            for nm in left:
                try:
                    os.stat(os.path.join(sub, nm))
                    real.append((nm, "REAL"))
                except OSError as e:
                    real.append((nm, "PHANTOM errno=%d" % e.errno))
            note(t, it, "leftover", len(left), real[:4])
        # cleanup
        try:
            os.rmdir(sub)
        except OSError as e:
            note(t, it, "rmdir", e.errno)

ths = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
for th in ths: th.start()
for th in ths: th.join()
print("done: %d fails / %d dir-cycles" % (len(fails), THREADS * ITERS), flush=True)
sys.exit(1 if fails else 0)
