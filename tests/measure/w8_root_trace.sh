#!/bin/bash
# Where a single mount-root mkdir spends 1.2 s, and how many names the
# root actually has. Attaches strace to the running efs-fuse (never
# starts one under it). A hung readdir or mkdir is cleared by a
# same-binary remount at the end.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-trace
preflight_or_die

say "== root getattr"
ssh_ 10 fcstor003 'cd /tmp/efs && ./efs-mgmt raft-getattr 172.16.223.57:19810 1' \
    | tee "$OUT/getattr.txt"

say "== ptrace and child count"
ssh_ 20 fcstor007 'echo ptrace_scope=$(sysctl -n kernel.yama.ptrace_scope)
    timeout -k 2 12 python3 -c "
import os, time
t=time.time(); n=0
for e in os.scandir(\"/tmp/efs-mount\"):
    n+=1
print(\"NCHILDREN %d %.3f\"%(n, time.time()-t))
" ; echo COUNT_RC=$?' | tee "$OUT/count.txt"

say "== nanosleep during one root mkdir"
ssh_ 25 fcstor007 'pid=$(pgrep -x efs-fuse | head -1); echo fuse=$pid
    rm -f /tmp/w8-ns.txt
    timeout 18 strace -f -tt -T -e trace=nanosleep,clock_nanosleep -p $pid -o /tmp/w8-ns.txt &
    sp=$!
    sleep 1.2
    timeout -k 2 12 python3 -c "
import os, tempfile, time
t=time.time()
try:
    d=tempfile.mkdtemp(prefix=\"w8t-\", dir=\"/tmp/efs-mount\")
    print(\"MKDIR_OK %.3f %s\"%(time.time()-t, d))
    t2=time.time()
    os.rmdir(d)
    print(\"RMDIR_OK %.3f\"%(time.time()-t2))
except OSError as e:
    print(\"ERR %s %.3f\"%(e, time.time()-t))
" ; echo PY_RC=$?
    kill $sp 2>/dev/null || true
    wait $sp 2>/dev/null || true
    pkill -x strace 2>/dev/null || true
    echo NS_LINES=$(grep -c nanosleep /tmp/w8-ns.txt || true)
    grep nanosleep /tmp/w8-ns.txt | head -40' | tee "$OUT/nanosleep.txt"

say "== long read/recv during a second root mkdir"
ssh_ 25 fcstor007 'pid=$(pgrep -x efs-fuse | head -1); echo fuse=$pid
    rm -f /tmp/w8-io.txt
    timeout 18 strace -f -tt -T -e trace=read,recvfrom,recvmsg,write,sendto,sendmsg -p $pid -o /tmp/w8-io.txt &
    sp=$!
    sleep 1.2
    timeout -k 2 12 python3 -c "
import os, tempfile, time
t=time.time()
try:
    d=tempfile.mkdtemp(prefix=\"w8u-\", dir=\"/tmp/efs-mount\")
    print(\"MKDIR_OK %.3f %s\"%(time.time()-t, d))
    t2=time.time()
    os.rmdir(d)
    print(\"RMDIR_OK %.3f\"%(time.time()-t2))
except OSError as e:
    print(\"ERR %s %.3f\"%(e, time.time()-t))
" ; echo PY_RC=$?
    kill $sp 2>/dev/null || true
    wait $sp 2>/dev/null || true
    pkill -x strace 2>/dev/null || true
    python3 - <<"PY"
import re
slow=[]
n=0
for line in open("/tmp/w8-io.txt","r",errors="replace"):
    m=re.search(r"<([0-9.]+)>$", line.rstrip())
    if not m: continue
    n+=1
    dt=float(m.group(1))
    if dt>=0.020:
        slow.append((dt, line.strip()[:180]))
slow.sort(reverse=True)
print("IO_LINES", n, "SLOW", len(slow))
for dt,s in slow[:25]:
    print("%.3f %s"%(dt,s))
PY' | tee "$OUT/io.txt"

say "== clear"
remount_clients fcstor007 "" > "$OUT/remount.txt" || true
echo DONE
