#!/usr/bin/env python3
"""w125_packed_edges.py MNT MGMT SEED: size and read edges of packed files (W125 closure C3).

 1. files of 0, 1, 2, 4095, 4096, 4097, 16383, 16384 and 16385 bytes (x20
    each), small sparse files (one byte at offset 1-15000), and files
    extended by truncate; wait for packing; check which are packed (<= 16 KiB
    and non-empty only) and that every byte reads back;
 2. read patterns on packed files: partial reads at random offsets, reads
    past EOF (short), O_DIRECT reads (4 KiB aligned buffer), and 32 threads
    reading files of the same segments at once;
 3. modify packed files: append past 16 KiB (must leave packing), small
    append, overwrite in the middle, truncate shorter and longer; verify,
    wait for repacking, verify again;
 4. delete everything.
Exit 1 on any mismatch or error.
"""
import mmap, os, random, subprocess, sys, threading, time
mnt, mgmt, seed = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.path.join(mnt, "edges"); os.makedirs(root, exist_ok=True)
fail = 0
def check(c, m):
    global fail
    if not c: fail += 1; print("FAIL", m)
def packed(p):
    out = subprocess.run([mgmt, "raft-getchunks", seed, str(os.stat(p).st_ino)], capture_output=True, text=True).stdout
    return "packed segment=" in out
def data(key, n): return random.Random(key).randbytes(n)
expect = {}
for size in (0, 1, 2, 4095, 4096, 4097, 16383, 16384, 16385):
    for k in range(20):
        p = os.path.join(root, "s%05d_%02d" % (size, k)); b = data(p, size)
        with open(p, "wb") as f: f.write(b)
        expect[p] = b
for k in range(20):                          # sparse: one byte after a hole
    p = os.path.join(root, "sparse_%02d" % k); off = random.Random(p).randint(1, 15000)
    with open(p, "wb") as f: f.seek(off); f.write(b"x")
    expect[p] = b"\0" * off + b"x"
for k in range(20):                          # extended by truncate
    p = os.path.join(root, "trunc_%02d" % k); b = data(p, 3000)
    with open(p, "wb") as f: f.write(b); f.truncate(9000)
    expect[p] = b + b"\0" * 6000
def wait_packed(paths, want, t_max=240):
    for t in range(t_max // 3):
        n = sum(packed(p) for p in paths)
        if n >= want: return n
        time.sleep(3)
    return sum(packed(p) for p in paths)
small = [p for p, b in expect.items() if 0 < len(b) <= 16384]
large = [p for p, b in expect.items() if len(b) > 16384]
empty = [p for p, b in expect.items() if len(b) == 0]
n = wait_packed(small, len(small))
print("packed: small %d/%d, >16KiB %d/%d (must be 0), empty %d/%d (must be 0)" % (
    n, len(small), sum(packed(p) for p in large), len(large), sum(packed(p) for p in empty), len(empty)))
check(n == len(small), "not all small files packed")
check(not any(packed(p) for p in large + empty), "large or empty file packed")
bad = sum(1 for p, b in expect.items() if open(p, "rb").read() != b)
check(bad == 0, "%d files read wrong" % bad); print("read back: bad=%d of %d" % (bad, len(expect)))
rng = random.Random(3); errs = 0
for p in rng.sample(small, 100):             # partial reads and past EOF
    b = expect[p]; off = rng.randrange(len(b)); ln = rng.randint(1, 20000)
    with open(p, "rb") as f:
        f.seek(off); got = f.read(ln)
        if got != b[off:off + ln]: errs += 1
        f.seek(len(b) + 10); errs += f.read(10) != b""
check(errs == 0, "%d partial/past-EOF reads wrong" % errs)
derr = 0
buf = mmap.mmap(-1, 1 << 20)
for p in rng.sample(small, 50):              # O_DIRECT
    try:
        fd = os.open(p, os.O_RDONLY | os.O_DIRECT)
        n = os.readv(fd, [buf]); os.close(fd)
        if buf[:n] != expect[p]: derr += 1
    except OSError as e:
        derr += 1; print("O_DIRECT", p, e)
check(derr == 0, "%d O_DIRECT reads wrong" % derr)
terr = [0]
def reader(i):
    r = random.Random(i)
    for _ in range(200):
        p = r.choice(small)
        if open(p, "rb").read() != expect[p]: terr[0] += 1
ts = [threading.Thread(target=reader, args=(i,)) for i in range(32)]
for t in ts: t.start()
for t in ts: t.join()
check(terr[0] == 0, "%d concurrent same-segment reads wrong" % terr[0])
print("reads: partial/EOF errors=%d O_DIRECT errors=%d concurrent errors=%d" % (errs, derr, terr[0]))
mod = rng.sample([p for p in small if packed(p)], 60)
for i, p in enumerate(mod):                  # modify packed files
    b = expect[p]
    with open(p, "r+b") as f:
        k = i % 5
        if k == 0: x = data(p + "big", 20000); f.seek(0, 2); f.write(x); b = b + x
        elif k == 1: x = data(p + "a", 500); f.seek(0, 2); f.write(x); b = b + x
        elif k == 2:
            off = len(b) // 2; x = data(p + "o", min(300, len(b) - off)); f.seek(off); f.write(x); b = b[:off] + x + b[off + len(x):]
        elif k == 3: f.truncate(len(b) // 3); b = b[:len(b) // 3]
        else: f.truncate(len(b) + 4000); b = b + b"\0" * 4000
    expect[p] = b
bad = sum(1 for p in mod if open(p, "rb").read() != expect[p])
check(bad == 0, "%d modified files wrong right after modification" % bad)
grown = [p for p in mod if len(expect[p]) > 16384]
small2 = [p for p in mod if 0 < len(expect[p]) <= 16384]
n2 = wait_packed(small2, len(small2))
bad2 = sum(1 for p in mod if open(p, "rb").read() != expect[p])
print("modified %d: wrong now=%d; repacked %d/%d; grown past 16KiB packed %d/%d (must be 0); wrong after repack=%d" % (
    len(mod), bad, n2, len(small2), sum(packed(p) for p in grown), len(grown), bad2))
check(n2 == len(small2), "modified small files not repacked")
check(not any(packed(p) for p in grown), "file grown past 16 KiB still packed")
check(bad2 == 0, "modified files wrong after repack")
for p in expect: os.unlink(p)
os.rmdir(root)
print("FAIL count %d" % fail); sys.exit(1 if fail else 0)
