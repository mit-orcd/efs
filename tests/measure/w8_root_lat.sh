#!/bin/bash
# Latency of one-at-a-time mkdir+rmdir in the mount root versus in a
# fresh directory. The traced pair was 3 ms then 1.12 s, and the 1.12 s
# was one recvfrom — this records which of those a run of 20 is.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-lat
preflight_or_die

raft() { ssh_ 8 fcstor003 'cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.57:19810' | grep 'group 0 '; }

say "== commit before"
raft | tee "$OUT/raft-before.txt"

ssh_ 15 fcstor007 "timeout -k 2 8 python3 -c 'import os; os.mkdir(\"$MNT/w8lat\")'; echo PARENT_RC=\$?" \
    | tee "$OUT/parent.txt"

say "== 20 root then 20 in the fresh directory"
ssh_ 60 fcstor007 'timeout -k 2 50 python3 - <<"PY"
import os, tempfile, time
mnt="/tmp/efs-mount"
def one(parent, tag):
    t=time.time()
    try:
        d=tempfile.mkdtemp(prefix="w8l-", dir=parent)
    except OSError as e:
        print("%s MKDIR_ERR %s %.3f"%(tag, e, time.time()-t))
        return
    tm=time.time()-t
    t=time.time()
    try:
        os.rmdir(d)
        print("%s %.3f %.3f"%(tag, tm, time.time()-t))
    except OSError as e:
        print("%s %.3f RMDIR_ERR %s %.3f"%(tag, tm, e, time.time()-t))
for i in range(20):
    one(mnt, "root")
sub=mnt+"/w8lat"
for i in range(20):
    one(sub, "sub")
PY' | tee "$OUT/lat.txt"

say "== commit after"
raft | tee "$OUT/raft-after.txt"
ssh_ 10 fcstor007 "rmdir $MNT/w8lat 2>/dev/null; echo cleaned" || true
echo DONE
python3 - "$OUT/lat.txt" <<'PY'
import sys
rows={"root":[],"sub":[]}
for line in open(sys.argv[1]):
    p=line.split()
    if len(p)<3 or p[0] not in rows: continue
    if "ERR" in line:
        print(line.rstrip()); continue
    rows[p[0]].append((float(p[1]), float(p[2])))
for k,v in rows.items():
    if not v: continue
    mk=sorted(x[0] for x in v); rm=sorted(x[1] for x in v)
    def pct(a,q):
        return a[min(len(a)-1, int(q*(len(a)-1)))]
    slow=sum(1 for x in mk if x>=0.2)
    print("%s n=%d mkdir min/med/max %.3f/%.3f/%.3f  rmdir min/med/max %.3f/%.3f/%.3f  mkdir>=0.2s %d"%(
        k,len(v),mk[0],pct(mk,0.5),mk[-1],rm[0],pct(rm,0.5),rm[-1],slow))
PY
