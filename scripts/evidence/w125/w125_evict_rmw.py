#!/usr/bin/env python3
"""w125_evict_rmw.py ROOT [--files N] [--rounds R] [--threads T] [--settle S]

Partial overwrite of closed files whose client chunk map may have been
evicted (W125 closure C1 follow-up, 2026-10-21). Writes N files of
256 KiB - 2 MiB (deterministic content), closes them, waits S seconds,
then R rounds: every file is reopened r+b and 1-4000 bytes are
overwritten at a random offset, then closed. Every file is verified
against its expected content after each round. Run with a small
EFS_CLIENT_META_MB on the efs-fuse mount so the staging evictor drops
chunk maps between close and reopen. Prints mismatches with the chunk
index (128 KiB) and whether that chunk reads as zeros outside the
overwritten bytes. Exit 1 on any mismatch or error.
"""
import argparse, os, random, sys, time
from concurrent.futures import ThreadPoolExecutor

CHUNK = 128 << 10
ap = argparse.ArgumentParser()
ap.add_argument("root"); ap.add_argument("--files", type=int, default=2000)
ap.add_argument("--rounds", type=int, default=3); ap.add_argument("--threads", type=int, default=8)
ap.add_argument("--settle", type=float, default=10)
a = ap.parse_args()
os.makedirs(a.root, exist_ok=True)
want = {}

def path(i): return os.path.join(a.root, "d%03d" % (i % 100), "f%06d" % i)
def make(i):
    r = random.Random(i)
    b = r.randbytes(r.randint(256 << 10, 2 << 20))
    os.makedirs(os.path.dirname(path(i)), exist_ok=True)
    with open(path(i), "wb") as f: f.write(b)
    return i, b
def modify(args):
    i, rnd = args
    r = random.Random("%d#%d" % (i, rnd)); b = bytearray(want[i])
    off = r.randrange(len(b)); x = r.randbytes(r.randint(1, 4000))[:len(b) - off]
    with open(path(i), "r+b") as f: f.seek(off); f.write(x)
    b[off:off + len(x)] = x
    return i, bytes(b), off, len(x)
def verify(i):
    try:
        with open(path(i), "rb") as f: got = f.read()
    except OSError as e:
        return "%s: %s" % (path(i), e)
    w = want[i]
    if got == w: return None
    if len(got) != len(w): return "%s: size %d, expected %d" % (path(i), len(got), len(w))
    bad = sorted({k // CHUNK for k in range(len(w)) if got[k] != w[k]})
    z = [ci for ci in bad if not got[ci * CHUNK:(ci + 1) * CHUNK].replace(b"\0", b"")]
    return "%s: bad chunks %s (all-zero %s)" % (path(i), bad, z)

errs = 0
with ThreadPoolExecutor(a.threads) as ex:
    t0 = time.time()
    for i, b in ex.map(make, range(a.files)): want[i] = b
    print("wrote %d files in %.0fs" % (a.files, time.time() - t0), flush=True)
    time.sleep(a.settle)
    for rnd in range(1, a.rounds + 1):
        n = 0
        for i, b, off, ln in ex.map(modify, [(i, rnd) for i in range(a.files)]):
            want[i] = b; n += 1
        bad = [m for m in ex.map(verify, range(a.files)) if m]
        errs += len(bad)
        print("round %d: modified %d, bad %d" % (rnd, n, len(bad)), flush=True)
        for m in bad[:10]: print("  " + m, flush=True)
        time.sleep(a.settle)
print("RESULT %s" % ("FAIL" if errs else "PASS"))
sys.exit(1 if errs else 0)
