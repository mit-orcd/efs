#!/usr/bin/env python3
"""ecopy-shaped burst: N threads each copy files of odd sizes with
write -> futimens(ns) -> close -> rename. Then stat every file and report
size / atime / mtime mismatches (same client). Server truth: stat the same
tree from another client afterwards (size_gate_check.py)."""
import os, sys, threading, time, json
d = sys.argv[1]
nthreads = int(sys.argv[2]) if len(sys.argv) > 2 else 32
per = int(sys.argv[3]) if len(sys.argv) > 3 else 100
sizes = [32398, 20480 + 2712, 1000, 131073, 300000, 4097, 65536 + 5, 131072 - 1]
os.makedirs(d, exist_ok=True)
A = 1780070101 * 10**9 + 301824970
M = 1733413852 * 10**9 + 314886393
exp = {}
lock = threading.Lock()
errs = []

def worker(t):
    sub = os.path.join(d, "t%02d" % t)
    os.makedirs(sub, exist_ok=True)
    for i in range(per):
        sz = sizes[(t + i) % len(sizes)]
        p = os.path.join(sub, "f%04d.tmp" % i)
        q = os.path.join(sub, "f%04d" % i)
        try:
            fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o664)
            off = 0
            while off < sz:
                n = os.write(fd, bytes([65 + (i % 26)]) * min(32768, sz - off))
                off += n
            os.utime(fd, ns=(A + i, M + i))
            os.close(fd)
            os.rename(p, q)
            with lock:
                exp[q] = (sz, A + i, M + i)
        except OSError as e:
            with lock:
                errs.append("%s: %s" % (p, e))

t0 = time.time()
ths = [threading.Thread(target=worker, args=(t,)) for t in range(nthreads)]
for th in ths:
    th.start()
for th in ths:
    th.join()
t1 = time.time()
bad = []
for q, (sz, a, m) in sorted(exp.items()):
    st = os.stat(q)
    if st.st_size != sz or st.st_atime_ns != a or st.st_mtime_ns != m:
        bad.append("%s size %d/%d atime %d/%d mtime %d/%d" % (q, st.st_size, sz,
                   st.st_atime_ns, a, st.st_mtime_ns, m))
print("files=%d threads=%d wall=%.1fs errors=%d mismatches(same client)=%d" %
      (len(exp), nthreads, t1 - t0, len(errs), len(bad)))
for e in errs[:10]:
    print("ERR", e)
for b in bad[:20]:
    print("BAD", b)
json.dump(exp, open(os.path.join("/tmp", "size_gate_expect.json"), "w"))
