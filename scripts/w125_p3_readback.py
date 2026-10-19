#!/usr/bin/env python3
# W125 P3 read-back check: w125_p3_readback.py write|verify DIR N writes N
# files of 1 B to 16 KiB whose bytes derive from the index, or verifies them.
"""W125 read-back check: write|verify N files whose content derives from the path."""
import hashlib, os, sys
mode, root, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
def content(i):
    size = 1 + (i * 7919) % 16384
    h = hashlib.sha256(str(i).encode()).digest()
    return (h * (size // 32 + 1))[:size]
bad = 0
for i in range(n):
    d = os.path.join(root, "d%03d" % (i // 500))
    p = os.path.join(d, "f%06d" % i)
    if mode == "write":
        os.makedirs(d, exist_ok=True)
        with open(p, "wb") as f: f.write(content(i))
    else:
        try:
            with open(p, "rb") as f: got = f.read()
        except OSError as e:
            got = None
            if bad < 3: print("read error", p, e)
        if got != content(i):
            bad += 1
            if bad <= 3 and got is not None: print("mismatch", p, len(got), len(content(i)))
print(mode, n, "bad=%d" % bad)
