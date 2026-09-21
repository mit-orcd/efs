#!/usr/bin/env python3
# md_latency.py — 20 ops each of mkdir/create/append/stat/unlink/rmdir in a
# fresh directory on the mount; min/med/max ms. Reference (idle 19810,
# Sep 21 02:43Z): mkdir 7.2, create+close 6.7, append+close 9.0, stat 0.4,
# unlink 1.8, rmdir 6.4 ms median. A median far above that on an idle
# cluster is a scheduling/lock bug (project-state rule), not fsync.
# Run on a client: python3 tests/measure/md_latency.py [/tmp/efs-mount]
import os, sys, time, statistics as st
mnt = sys.argv[1] if len(sys.argv) > 1 else "/tmp/efs-mount"
base = "%s/mdlat-%d" % (mnt, os.getpid())
os.mkdir(base)
def run(name, fn, n=20):
    ts = []
    for i in range(n):
        t = time.time(); fn(i); ts.append((time.time() - t) * 1000)
    print("%-18s min %6.1f med %6.1f max %6.1f ms" % (name, min(ts), st.median(ts), max(ts)))
run("mkdir", lambda i: os.mkdir("%s/d%d" % (base, i)))
def cr(i):
    fd = os.open("%s/f%d" % (base, i), os.O_CREAT | os.O_WRONLY, 0o644); os.write(fd, b"x"); os.close(fd)
run("create+1B+close", cr)
def ap(i):
    fd = os.open("%s/f%d" % (base, i), os.O_WRONLY | os.O_APPEND); os.write(fd, b"y"); os.close(fd)
run("append+close", ap)
run("stat", lambda i: os.stat("%s/f%d" % (base, i)))
run("unlink", lambda i: os.unlink("%s/f%d" % (base, i)))
run("rmdir", lambda i: os.rmdir("%s/d%d" % (base, i)))
os.rmdir(base)
