#!/usr/bin/env python3
"""W125 P5 modification matrix on packed files.

w125_p5_modify.py DIR PHASE   PHASE: write | modify | verify

write:  200 files per case (1-12 KiB, content from the index) in DIR/<case>/
modify: applies each case's modification to every file of the case
verify: checks every file against the model (rebuilt deterministically)
Cases: keep, overwrite, pwrite, append, shrink, zero, extend, punch, grow,
shrink_extend. efs does not support FALLOC_FL_PUNCH_HOLE for any file
(efs_fuse.c fallocate): the punch case expects EOPNOTSUPP and unchanged
bytes; holes come from extending truncates (extend, shrink_extend).
"""
import ctypes, hashlib, os, sys
root, phase = sys.argv[1], sys.argv[2]
N = 200
CASES = ["keep", "overwrite", "pwrite", "append", "shrink", "zero", "extend",
         "punch", "grow", "shrink_extend"]
libc = ctypes.CDLL(None, use_errno=True)
FALLOC_FL_KEEP_SIZE, FALLOC_FL_PUNCH_HOLE = 1, 2
def data(tag, i, size):
    h = hashlib.sha256(f"{tag}:{i}".encode()).digest()
    return (h * (size // 32 + 1))[:size]
def base(i): return data("base", i, 1024 + (i * 389) % 11264)
def model(case, i):
    b = bytearray(base(i))
    if case == "overwrite": return data("new", i, 700 + i % 3000)
    if case == "pwrite":
        off = len(b) // 3; p = data("mid", i, 300)
        b[off:off + 300] = p; return bytes(b)
    if case == "append": return bytes(b) + data("app", i, 500)
    if case == "shrink": return bytes(b[:len(b) // 2])
    if case == "zero": return data("after0", i, 200)
    if case == "extend": return bytes(b) + bytes(4096)
    if case == "punch": return bytes(b)  # EOPNOTSUPP: unchanged
    if case == "grow": return bytes(b) + data("grow", i, 20000)
    if case == "shrink_extend": return bytes(b[:len(b) // 2]) + bytes(len(b) - len(b) // 2)
    return bytes(b)
def path(case, i): return os.path.join(root, case, "f%04d" % i)
bad = 0
for case in CASES:
    os.makedirs(os.path.join(root, case), exist_ok=True)
    for i in range(N):
        p = path(case, i); b = base(i)
        if phase == "write":
            with open(p, "wb") as f: f.write(b)
        elif phase == "modify":
            if case == "overwrite":
                with open(p, "wb") as f: f.write(model(case, i))
            elif case == "pwrite":
                with open(p, "r+b") as f: f.seek(len(b) // 3); f.write(data("mid", i, 300))
            elif case == "append":
                with open(p, "ab") as f: f.write(data("app", i, 500))
            elif case == "shrink": os.truncate(p, len(b) // 2)
            elif case == "zero":
                os.truncate(p, 0)
                with open(p, "r+b") as f: f.write(data("after0", i, 200))
            elif case == "extend": os.truncate(p, len(b) + 4096)
            elif case == "punch":
                off = (len(b) // 4) & ~4095
                if off + 4096 <= len(b):
                    fd = os.open(p, os.O_RDWR)
                    r = libc.fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                       ctypes.c_long(off), ctypes.c_long(4096))
                    os.close(fd)
                    err = ctypes.get_errno()
                    if r == 0 or err != 95:  # must be refused with EOPNOTSUPP
                        bad += 1
                        if bad <= 3: print("punch: expected EOPNOTSUPP", p, r, err)
            elif case == "grow":
                with open(p, "ab") as f: f.write(data("grow", i, 20000))
            elif case == "shrink_extend":
                os.truncate(p, len(b) // 2); os.truncate(p, len(b))
        else:
            want = model(case, i)
            try:
                with open(p, "rb") as f: got = f.read()
            except OSError as e:
                got = None
            if got != want:
                bad += 1
                if bad <= 5:
                    print("MISMATCH", case, i, None if got is None else len(got), len(want))
print(phase, "cases=%d files=%d bad=%d" % (len(CASES), len(CASES) * N, bad))
sys.exit(1 if bad else 0)
