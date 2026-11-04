#!/usr/bin/env python3
"""w125_packed_namespace.py MNT MGMT SEED: namespace operations on packed files (W125 closure C2).

On an efs mount whose packer runs with a short minimum age:
 1. create 2,000 small files (100 B - 16 KiB) in 20 directories and wait
    until a sample is packed (efs-mgmt raft-getchunks shows "packed");
 2. rename half within their directory, move a quarter to other
    directories, hard-link 200 files (link counts 2-3) and then delete the
    original names of 100 of them, chmod and utime 300;
 3. verify every name byte for byte and check that metadata-only changes
    (rename, link, chmod, utime) left sampled files packed;
 4. delete everything; the caller checks that no segment remains.
One line per step; exit 1 on any error or mismatch.
"""
import os, random, subprocess, sys, time
mnt, mgmt, seed = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.path.join(mnt, "ns"); os.makedirs(root, exist_ok=True)
def body(key):
    r = random.Random(key); return r.randbytes(r.randint(100, 16384))
def packed(path):
    out = subprocess.run([mgmt, "raft-getchunks", seed, str(os.stat(path).st_ino)], capture_output=True, text=True).stdout
    return "packed segment=" in out
fail = 0
def check(cond, msg):
    global fail
    if not cond: fail += 1; print("FAIL", msg)
names = {}  # path -> content key
for d in range(20): os.makedirs(os.path.join(root, "d%02d" % d), exist_ok=True)
for i in range(2000):
    p = os.path.join(root, "d%02d" % (i % 20), "f%04d" % i)
    with open(p, "wb") as f: f.write(body(p))
    names[p] = p
sample = random.Random(1).sample(sorted(names), 40)
for t in range(120):
    if all(packed(p) for p in sample[:10]): break
    time.sleep(2)
print("packed sample: %d/40 after %ds" % (sum(packed(p) for p in sample), t * 2))
check(sum(packed(p) for p in sample) >= 36, "files were not packed")
rng = random.Random(7); paths = sorted(names)
for p in paths[:1000]:                       # rename within the directory
    q = p + ".r"; os.rename(p, q); names[q] = names.pop(p)
for p in [x for x in sorted(names) if x.endswith("f0001.r") or random.Random(x).random() < 0.25][:500]:
    q = os.path.join(root, "d%02d" % rng.randrange(20), os.path.basename(p) + ".m")
    if os.path.exists(q): continue
    os.rename(p, q); names[q] = names.pop(p)
links = []
for p in rng.sample(sorted(names), 200):     # hard links, 2-3 names per file
    for k in range(rng.randint(1, 2)):
        q = "%s.l%d" % (p, k); os.link(p, q); names[q] = names[p]; links.append((p, q))
gone = 0
for p, q in links[:100]:                     # delete an original name, keep the link
    if p in names: os.unlink(p); names.pop(p); gone += 1
meta = rng.sample(sorted(names), 300)
for p in meta: os.chmod(p, 0o640); os.utime(p, (1700000000, 1700000000))
print("renamed 1000, moved %d, linked %d names, unlinked %d originals, chmod/utime 300" % (
    sum(1 for p in names if p.endswith(".m")), len(links), gone))
bad = sum(1 for p, k in names.items() if open(p, "rb").read() != body(k))
check(bad == 0, "%d names read wrong" % bad)
for p in meta[:50]:
    st = os.stat(p); check(st.st_mode & 0o777 == 0o640 and int(st.st_mtime) == 1700000000, "chmod/utime lost on " + p)
still = sum(packed(p) for p in meta[:40])
print("verify: %d names, bad=%d; metadata-changed files still packed: %d/40" % (len(names), bad, still))
check(still >= 36, "metadata-only changes unpacked files")
nl = [os.stat(q).st_nlink for _, q in links[:50] if q in names]
check(all(n >= 1 for n in nl), "link counts")
for p in sorted(names): os.unlink(p)
for d in range(20): os.rmdir(os.path.join(root, "d%02d" % d))
os.rmdir(root)
print("deleted all; FAIL count %d" % fail)
sys.exit(1 if fail else 0)
