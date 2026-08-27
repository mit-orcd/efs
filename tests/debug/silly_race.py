#!/usr/bin/env python3
# Repro for the FUSE silly-rename (.fuse_hidden) flake: close a file then
# unlink it IMMEDIATELY (tight window) while a churn thread keeps the kernel
# release workqueue busy, so a deferred release lands after the unlink.
# A leftover .fuse_hidden<hex> in listdir = the kernel silly-renamed an
# open-at-unlink file.
import os, sys, threading, time

MNT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs-mount"
THREADS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
ITERS = int(sys.argv[3]) if len(sys.argv) > 3 else 200

fails = []
lock = threading.Lock()
stop = False

def note(*a):
    with lock:
        fails.append(a)
        print("FAIL", *a, flush=True)

def worker(t):
    d = os.path.join(MNT, "sz-%d" % t)
    os.makedirs(d, exist_ok=True)
    for it in range(ITERS):
        p = os.path.join(d, "f%d" % it)
        try:
            with open(p, "wb") as f:
                f.write(b"data")
            with open(p, "rb") as f:
                f.read()
            os.unlink(p)  # tight window: unlink right after last close
        except OSError as e:
            note(t, it, "op errno=%d" % e.errno)
            continue
        # any .fuse_hidden in the dir = a silly-rename leaked
        try:
            hid = [x for x in os.listdir(d) if x.startswith(".fuse_hidden")]
        except OSError:
            hid = []
        if hid:
            det = []
            for nm in hid:
                try:
                    st = os.stat(os.path.join(d, nm))
                    det.append("%s ino=%d nlink=%d" % (nm, st.st_ino, st.st_nlink))
                except OSError as e:
                    det.append("%s PHANTOM errno=%d" % (nm, e.errno))
            note(t, it, "silly", det)

def churn():
    # keep the kernel release/writeback workqueues busy
    d = os.path.join(MNT, "sz-churn")
    os.makedirs(d, exist_ok=True)
    i = 0
    while not stop:
        p = os.path.join(d, "c%d" % (i % 64))
        try:
            with open(p, "wb") as f:
                f.write(b"x" * 65536)
            os.unlink(p)
        except OSError:
            pass
        i += 1

ct = threading.Thread(target=churn, daemon=True)
ct.start()
ths = [threading.Thread(target=worker, args=(i,)) for i in range(THREADS)]
t0 = time.time()
for th in ths: th.start()
for th in ths: th.join()
stop = True
ct.join(timeout=5)
print("done: %d fails / %d file-cycles in %.1fs" %
      (len(fails), THREADS * ITERS, time.time() - t0), flush=True)
sys.exit(1 if fails else 0)
