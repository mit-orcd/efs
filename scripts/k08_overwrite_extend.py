#!/usr/bin/env python3
"""k08_overwrite_extend.py ROOT SECONDS [--threads 16] [--size 3600000] [--dump DIR]

W148 / alpha K8 focused stress: each thread owns its files (no oracle
races). Per round on one file: read and verify (warms the client caches),
overwrite 1..4000 bytes in the middle at the same size, close, optionally
read and verify, truncate up by 1..8000 bytes, close, read and verify.
Every 8 rounds the file is rewritten from scratch so it does not grow
without bound. Content is a pure function of (file, round), so the
expected bytes are exact. A mismatching read is saved under DIR/N with a
sidecar (file, round, phase, expected/actual size and sha256, the first
differing offset and the time); the read is then repeated until it
settles (up to 10 s) and the sidecar records how long that took.
Exit 1 on any mismatch or error.
"""
import argparse, hashlib, json, os, random, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("root"); ap.add_argument("seconds", type=float)
ap.add_argument("--threads", type=int, default=16)
ap.add_argument("--files", type=int, default=4, help="files per thread")
ap.add_argument("--size", type=int, default=3600000)
ap.add_argument("--mid-read", type=int, default=1, help="read between overwrite and extend")
ap.add_argument("--dump")
a = ap.parse_args()
lock = threading.Lock()
st = dict(rounds=0, reads=0, mismatch=0, errors=0)
errs, dumps = [], [0]

def sha(b): return hashlib.sha256(b).hexdigest()

def first_diff(x, y):
    for i in range(min(len(x), len(y))):
        if x[i] != y[i]: return i
    return min(len(x), len(y))

def dump(path, rnd, phase, want, got, settle):
    with lock:
        st["mismatch"] += 1
        if len(errs) < 40:
            errs.append("%s r%d %s: got %d bytes sha %s, want %d sha %s, diff@%d settle=%s" % (
                path, rnd, phase, len(got), sha(got)[:16], len(want), sha(want)[:16],
                first_diff(want, got), settle))
        dumps[0] += 1; n = dumps[0]
    if not a.dump or n > 50: return
    d = os.path.join(a.dump, str(n)); os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "actual"), "wb") as f: f.write(got)
    with open(os.path.join(d, "expected"), "wb") as f: f.write(want)
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(dict(file=path, round=rnd, phase=phase, expected_len=len(want),
                       actual_len=len(got), expected_sha256=sha(want), actual_sha256=sha(got),
                       first_diff=first_diff(want, got), time=time.time(), settle=settle,
                       ino=os.stat(path).st_ino), f, indent=1)

def check(path, rnd, phase, want):
    with open(path, "rb") as f: got = f.read()
    with lock: st["reads"] += 1
    if got == want: return
    t0 = time.time(); settle = None
    while time.time() - t0 < 10:
        time.sleep(0.05)
        with open(path, "rb") as f: again = f.read()
        if again == want: settle = round(time.time() - t0, 3); break
    dump(path, rnd, phase, want, got, settle)

def worker(t):
    r = random.Random(t)
    files = []
    for i in range(a.files):
        p = os.path.join(a.root, "t%02d_f%d" % (t, i))
        files.append([p, None, 0])
    deadline = time.time() + a.seconds
    while time.time() < deadline:
        fe = r.choice(files); p = fe[0]
        try:
            if fe[1] is None or fe[2] % 8 == 0:
                data = r.randbytes(a.size + r.randint(0, 65536))
                with open(p, "wb") as f: f.write(data)
                fe[1] = data
            data = fe[1]
            check(p, fe[2], "pre", data)
            off = r.randrange(len(data) // 4, 3 * len(data) // 4)
            x = r.randbytes(r.randint(1, 4000))
            with open(p, "r+b") as f: f.seek(off); f.write(x)
            data = data[:off] + x + data[off + len(x):]
            if a.mid_read: check(p, fe[2], "overwrite", data)
            grow = r.randint(1, 8000)
            with open(p, "r+b") as f: f.truncate(len(data) + grow)
            data = data + b"\0" * grow
            check(p, fe[2], "extend", data)
            fe[1] = data; fe[2] += 1
            with lock: st["rounds"] += 1
        except OSError as e:
            with lock:
                st["errors"] += 1
                if len(errs) < 40: errs.append("%s: %s" % (p, e))
            fe[1] = None

os.makedirs(a.root, exist_ok=True)
th = [threading.Thread(target=worker, args=(i,)) for i in range(a.threads)]
for x in th: x.start()
t0 = time.time()
while any(x.is_alive() for x in th):
    time.sleep(10)
    with lock: s = dict(st)
    print("t=%.0fs rounds=%d reads=%d mismatch=%d errors=%d" % (
        time.time() - t0, s["rounds"], s["reads"], s["mismatch"], s["errors"]), flush=True)
for e in errs: print("ERR", e)
print("final rounds=%d reads=%d mismatch=%d errors=%d" % (st["rounds"], st["reads"], st["mismatch"], st["errors"]))
sys.exit(1 if st["mismatch"] or st["errors"] else 0)
