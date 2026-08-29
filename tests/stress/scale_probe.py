#!/usr/bin/env python3
"""Measure per-op latency against a populated table, on an otherwise idle mount.

Run between growth steps. Every operation here is O(1) work in principle, so
any growth in these numbers as the table gets bigger is the scaling defect we
are looking for -- a linear scan, a rehash, a table walk under a lock.

    scale_probe.py MNT TAG NFILES FANOUT [ITERS]

NFILES/FANOUT describe the population that already exists under scale/TAG, so
the probe can address random existing paths without listing anything (a listing
would itself be O(dir) and would pollute the measurement).
"""
import os
import random
import shutil
import statistics
import sys
import time


def pct(xs, p):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    k = min(len(xs) - 1, int(round((p / 100.0) * (len(xs) - 1))))
    return xs[k] * 1000.0


def report(name, xs):
    print("%-14s p50=%8.3f p99=%8.3f max=%8.3f n=%d"
          % (name, pct(xs, 50), pct(xs, 99), pct(xs, 100), len(xs)))
    sys.stdout.flush()


def main():
    mnt, tag = sys.argv[1], sys.argv[2]
    nfiles, fanout = int(sys.argv[3]), int(sys.argv[4])
    iters = int(sys.argv[5]) if len(sys.argv) > 5 else 200

    root = os.path.join(mnt, "scale", tag)
    rnd = random.Random(1234)

    def existing():
        i = rnd.randrange(nfiles)
        return os.path.join(root, "d%06d" % (i // fanout), "f%09d" % i)

    # stat of a random existing file: the pure lookup+getattr path.
    xs = []
    for _ in range(iters):
        p = existing()
        t = time.time()
        try:
            os.stat(p)
        except OSError:
            continue
        xs.append(time.time() - t)
    report("stat", xs)

    # create: the op that dominates the burst, and the one that has to allocate
    # an inode out of a table that is now large.
    # Unique per invocation: a probe that dies mid-run would otherwise leave
    # files behind that collide with the next step's O_EXCL creates.
    pdir = os.path.join(root, "probe-%d-%d" % (os.getpid(), int(time.time())))
    shutil.rmtree(pdir, ignore_errors=True)
    os.makedirs(pdir, exist_ok=True)
    xs = []
    for i in range(iters):
        p = os.path.join(pdir, "p%d.%d" % (os.getpid(), i))
        t = time.time()
        fd = os.open(p, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        os.close(fd)
        xs.append(time.time() - t)
    report("create", xs)

    # unlink: fans across every loaded shard table, so it is the op most
    # exposed to a per-table scan.
    xs = []
    for i in range(iters):
        p = os.path.join(pdir, "p%d.%d" % (os.getpid(), i))
        t = time.time()
        try:
            os.unlink(p)
        except OSError:
            continue
        xs.append(time.time() - t)
    report("unlink", xs)

    # readdir of one populated directory: should track FANOUT, not the table.
    xs = []
    for _ in range(min(iters, 30)):
        d = os.path.join(root, "d%06d" % rnd.randrange(max(1, nfiles // fanout)))
        t = time.time()
        try:
            n = len(os.listdir(d))
        except OSError:
            continue
        xs.append(time.time() - t)
    report("readdir", xs)

    shutil.rmtree(pdir, ignore_errors=True)
    os._exit(0)


main()
