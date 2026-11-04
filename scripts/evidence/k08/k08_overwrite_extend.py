#!/usr/bin/env python3
"""k08_overwrite_extend.py ROOT SECONDS [--threads 16] [--size 3600000] [--dump DIR]
                         [--mode overwrite-extend|mix] [--mgmt 'EFS-MGMT ADDR']

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
--mode seq runs the sequence common to every W148 event on each file:
create, close, then on r+b descriptors truncate(0) and write a new body,
truncate up by 1..8000 bytes, overwrite 1..4000 bytes in the middle at the
same size, each closed and verified; a file's next stage waits --gap
seconds, so many files (--files) interleave as in the W125 workload.
--mode mix applies, per round, one random W125 modifier operation on an
r+b descriptor (rewrite = truncate(0) then write, append, shrink, extend,
overwrite in the middle at the same size) and verifies after each; a file
past 6 MiB or under 4 KiB is rewritten. --mgmt saves 'MGMT raft-getattr
INO' and 'MGMT raft-getchunks INO' output next to each dump.
Exit 1 on any mismatch or error.
"""
import argparse, hashlib, json, os, random, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("root"); ap.add_argument("seconds", type=float)
ap.add_argument("--threads", type=int, default=16)
ap.add_argument("--files", type=int, default=4, help="files per thread")
ap.add_argument("--size", type=int, default=3600000)
ap.add_argument("--mid-read", type=int, default=1, help="read between overwrite and extend")
ap.add_argument("--dump")
ap.add_argument("--mode", choices=("overwrite-extend", "mix", "seq"), default="overwrite-extend")
ap.add_argument("--mgmt")
ap.add_argument("--gap", type=float, default=0.0, help="seq: seconds between a file's stages")
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
    if a.mgmt:
        for cmd in ("raft-getattr", "raft-getchunks"):
            try:
                m = a.mgmt.split()
                r = subprocess.run([m[0], cmd] + m[1:] + [str(os.stat(path).st_ino)],
                                   capture_output=True, text=True, timeout=30)
                out = r.stdout + r.stderr + "rc=%d\n" % r.returncode
            except (OSError, subprocess.SubprocessError) as e:
                out = "error %s\n" % e
            with open(os.path.join(d, cmd + ".txt"), "w") as f: f.write(out)
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

def seq_step(r, fe):
    """One stage of the W148 sequence; fe[3] is the stage, fe[4] the time
    the next stage may run (--gap seconds after this one)."""
    p, stage = fe[0], fe[3]
    if stage == 0:
        data = r.randbytes(int(r.uniform(0.3, 1.0) * a.size))
        with open(p, "wb") as f: f.write(data)
        op = "create"
    elif stage == 1:
        data = r.randbytes(int(r.uniform(0.3, 1.0) * a.size))
        with open(p, "r+b") as f: f.truncate(0); f.write(data)
        op = "rewrite"
    elif stage == 2:
        g = r.randint(1, 8000)
        with open(p, "r+b") as f: f.truncate(len(fe[1]) + g)
        data = fe[1] + b"\0" * g; op = "extend"
    else:
        old = fe[1]; off = r.randrange(len(old) // 4, 3 * len(old) // 4)
        x = r.randbytes(r.randint(1, 4000))
        with open(p, "r+b") as f: f.seek(off); f.write(x)
        data = old[:off] + x + old[off + len(x):]; op = "overwrite"
    fe[1] = data
    check(p, fe[2], op, data)
    fe[3] = (stage + 1) % 4
    fe[4] = time.time() + a.gap
    if fe[3] == 0:
        fe[2] += 1
        with lock: st["rounds"] += 1

def mix_round(r, fe):
    p, data = fe[0], fe[1]
    if data is None or len(data) > (6 << 20) or len(data) < 4096:
        data = r.randbytes(int(r.uniform(0.2, 1.0) * a.size))
        with open(p, "wb") as f: f.write(data)
        fe[1] = data
        check(p, fe[2], "create", data)
    k = r.randrange(5)
    with open(p, "r+b") as f:
        if k == 0:
            nd = r.randbytes(int(r.uniform(0.2, 1.0) * a.size)); f.truncate(0); f.write(nd); op = "rewrite"
        elif k == 1:
            x = r.randbytes(r.randint(1, 20000)); f.seek(0, 2); f.write(x); nd = data + x; op = "append"
        elif k == 2:
            n = int(len(data) * r.uniform(0.1, 0.9)); f.truncate(n); nd = data[:n]; op = "shrink"
        elif k == 3:
            g = r.randint(1, 8000); f.truncate(len(data) + g); nd = data + b"\0" * g; op = "extend"
        else:
            off = int(len(data) * r.random()); x = r.randbytes(r.randint(1, 4000))[:max(0, len(data) - off)]
            f.seek(off); f.write(x); nd = data[:off] + x + data[off + len(x):]; op = "overwrite"
    fe[1] = nd
    check(p, fe[2], op, nd)
    fe[2] += 1
    with lock: st["rounds"] += 1

def worker(t):
    r = random.Random(t)
    files = []
    for i in range(a.files):
        p = os.path.join(a.root, "t%02d_f%d" % (t, i))
        files.append([p, None, 0, 0, 0.0])
    deadline = time.time() + a.seconds
    while time.time() < deadline:
        fe = r.choice(files); p = fe[0]
        try:
            if a.mode == "mix":
                mix_round(r, fe)
                continue
            if a.mode == "seq":
                ready = [x for x in files if x[4] <= time.time()]
                if not ready:
                    time.sleep(0.05); continue
                seq_step(r, r.choice(ready))
                continue
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
            fe[1] = None; fe[3] = 0

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
