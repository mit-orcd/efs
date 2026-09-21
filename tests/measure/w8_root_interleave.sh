#!/bin/bash
# Interleave one mkdir in the mount root with one in a fresh directory,
# so a 1 s stall cannot be "the root phase happened to be slow."
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-interleave
preflight_or_die
ssh_ 12 fcstor007 "timeout -k 2 8 python3 -c 'import os; os.mkdir(\"$MNT/w8ix\")'; echo PARENT_RC=\$?" \
    | tee "$OUT/parent.txt"
ssh_ 60 fcstor007 'timeout -k 2 50 python3 - <<"PY"
import os, tempfile, time
mnt="/tmp/efs-mount"
sub=mnt+"/w8ix"
def one(parent, tag):
    t=time.time()
    try:
        d=tempfile.mkdtemp(prefix="w8i-", dir=parent)
    except OSError as e:
        print("%s MKDIR_ERR %s %.3f"%(tag, e, time.time()-t)); return
    tm=time.time()-t
    t=time.time()
    try:
        os.rmdir(d)
        print("%s %.3f %.3f"%(tag, tm, time.time()-t))
    except OSError as e:
        print("%s %.3f RMDIR_ERR %s %.3f"%(tag, tm, e, time.time()-t))
for i in range(16):
    one(mnt, "root")
    one(sub, "sub")
PY' | tee "$OUT/lat.txt"
ssh_ 10 fcstor007 "rmdir $MNT/w8ix 2>/dev/null; echo cleaned" || true
echo DONE
python3 - "$OUT/lat.txt" <<'PY'
import sys
rows={"root":[],"sub":[]}
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<3 or p[0] not in rows or "ERR" in line:
        if "ERR" in line: print(line.rstrip())
        continue
    rows[p[0]].append((float(p[1]), float(p[2])))
for k,v in rows.items():
    mk=sorted(x[0] for x in v)
    slow=sum(1 for x in mk if x>=0.2)
    print("%s n=%d mkdir min/med/max %.3f/%.3f/%.3f  slow=%d"%(
        k,len(v),mk[0],mk[len(mk)//2],mk[-1],slow))
PY
