import hashlib, os, sys, random
mode, root, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
bad = 0
for i in range(n):
    d = os.path.join(root, "d%03d" % (i % 200)); p = os.path.join(d, "f%06d" % i)
    r = random.Random(i); size = r.choice([100, 1000, 4096, 6000, 12000, 16384])
    data = r.randbytes(size)
    if mode == "write":
        os.makedirs(d, exist_ok=True)
        with open(p, "wb") as f: f.write(data)
    elif open(p, "rb").read() != data:
        bad += 1
print(mode, n, "bad", bad)
sys.exit(1 if bad else 0)
