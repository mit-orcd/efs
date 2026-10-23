#!/usr/bin/env python3
"""K28 workload: write large and small files with deterministic content,
record sha256 per file into a manifest, or verify a manifest.

    gate_k28_workload.py large  MNT MANIFEST N MIB [DIRSUFFIX]  # N files of MIB MiB in MNT/large<DIRSUFFIX>
    gate_k28_workload.py small  MNT MANIFEST N NAME             # N files of 1 B-16 KiB in MNT/small/NAME
    gate_k28_workload.py verify MNT MANIFEST                    # files deleted on purpose are skipped

Use a new DIRSUFFIX per phase: rewriting a name makes its older manifest
line stale. Exit 1 on any mismatch or read error."""
import hashlib, os, sys, random
mode, mnt, man = sys.argv[1], sys.argv[2], sys.argv[3]
def data(seed, n):
    r = random.Random(seed); return r.randbytes(n)
if mode in ("large", "small"):
    out = open(man, "a")
    if mode == "large":
        d = os.path.join(mnt, "large" + (sys.argv[6] if len(sys.argv) > 6 else "")); os.makedirs(d, exist_ok=True)
        for i in range(int(sys.argv[4])):
            p = os.path.join(d, f"f{i:03d}"); h = hashlib.sha256()
            with open(p, "wb") as f:
                for c in range(int(sys.argv[5])):     # MiB
                    b = data(i * 100000 + c, 1 << 20); f.write(b); h.update(b)
            out.write(f"{p} {h.hexdigest()}\n"); out.flush()
    else:
        d = os.path.join(mnt, "small", sys.argv[5]); os.makedirs(d, exist_ok=True)
        for i in range(int(sys.argv[4])):
            p = os.path.join(d, f"s{i:05d}")
            b = data(int(hashlib.sha256(f"{sys.argv[5]}:{i}".encode()).hexdigest()[:8], 16), random.Random(i).randint(1, 16384))
            with open(p, "wb") as f: f.write(b)
            out.write(f"{p} {hashlib.sha256(b).hexdigest()}\n")
        out.flush()
elif mode == "verify":
    bad = n = 0; missing = 0
    for line in open(man):
        p, s = line.split()
        if not os.path.exists(p): continue   # deleted on purpose
        n += 1
        h = hashlib.sha256()
        try:
            with open(p, "rb") as f:
                while True:
                    b = f.read(1 << 22)
                    if not b: break
                    h.update(b)
        except OSError as e:
            bad += 1; print("ERROR", p, e); continue
        if h.hexdigest() != s: bad += 1; print("MISMATCH", p)
    print(f"verify files={n} mismatches={bad}")
    sys.exit(1 if bad else 0)
