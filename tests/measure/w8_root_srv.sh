#!/bin/bash
# Where the server spends the 1 s on a mount-root mkdir. Attaches strace
# to the RUNNING efsd on the client's dual host and both group leaders
# for 22 s, runs 12 root mkdir+rmdir pairs from fcstor007 with wall-clock
# stamps, then lists every server syscall that started inside a slow
# client op and lasted >= 0.15 s. Never starts a daemon under strace.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-srv
preflight_or_die

say "== which server holds the client's metadata socket"
ssh_ 10 fcstor007 'ss -tnp 2>/dev/null | grep -F "efs-fuse" | grep -oE "172\.16\.223\.[0-9]+:19810" | sort | uniq -c' \
    | tee "$OUT/client-peers.txt"

SRV="fcstor003 fcstor004 fcstor005 fcstor006"
say "== strace efsd on $SRV for 22 s"
for h in $SRV; do
    ssh_ 40 "$h" 'pid=$(pgrep -x efsd | head -1); echo '"$h"' efsd=$pid
        rm -f /tmp/w8-efsd.txt
        timeout -s INT 22 strace -f -tt -T -e trace=recvfrom,recvmsg,read,poll,futex,nanosleep,clock_nanosleep,fdatasync,fsync,pwrite64,write,sendto,sendmsg,epoll_wait -p $pid -o /tmp/w8-efsd.txt >/dev/null 2>&1
        python3 - <<"PY"
import re
out=[]
for line in open("/tmp/w8-efsd.txt","r",errors="replace"):
    m=re.match(r"(\d+)\s+(\d+):(\d+):(\d+\.\d+)\s+(?:<\.\.\. )?(\w+).*<(\d+\.\d+)>$", line.rstrip())
    if not m: continue
    tid,hh,mm,ss,sc,dur=m.groups()
    dur=float(dur)
    if dur<0.15: continue
    end=int(hh)*3600+int(mm)*60+float(ss)
    out.append((end-dur, dur, tid, sc, line.strip()[:150]))
out.sort()
print("LONG", len(out))
for st,dur,tid,sc,l in out:
    print("%.3f %.3f %s %s | %s"%(st,dur,tid,sc,l))
PY' > "$OUT/srv-$h.txt" 2>&1 &
done
sleep 3

say "== 12 root mkdir+rmdir with wall stamps"
ssh_ 30 fcstor007 'timeout -k 2 20 python3 - <<"PY"
import os, tempfile, time
mnt="/tmp/efs-mount"
def wall(t):
    lt=time.localtime(t); return lt.tm_hour*3600+lt.tm_min*60+lt.tm_sec+(t-int(t))
for i in range(12):
    t0=time.time()
    try:
        d=tempfile.mkdtemp(prefix="w8s-", dir=mnt)
    except OSError as e:
        print("MKDIR_ERR %s"%e); continue
    t1=time.time()
    try:
        os.rmdir(d); t2=time.time()
        print("op mkdir %.3f %.3f %.3f"%(wall(t0), wall(t1), t1-t0))
        print("op rmdir %.3f %.3f %.3f"%(wall(t1), wall(t2), t2-t1))
    except OSError as e:
        print("RMDIR_ERR %s"%e)
    time.sleep(0.3)
PY' | tee "$OUT/client-ops.txt"
wait
for h in $SRV; do say "== $h"; head -3 "$OUT/srv-$h.txt"; done

say "== correlate: server syscalls starting inside a slow client op"
python3 - "$OUT" $SRV <<'PY'
import sys, re
out=sys.argv[1]; hosts=sys.argv[2:]
ops=[]
for line in open(out+"/client-ops.txt"):
    p=line.split()
    if len(p)==5 and p[0]=="op":
        ops.append((p[1], float(p[2]), float(p[3]), float(p[4])))
slow=[o for o in ops if o[3]>=0.5]
print("client ops", len(ops), "slow", len(slow))
for h in hosts:
    rows=[]
    for line in open(out+"/srv-%s.txt"%h, errors="replace"):
        p=line.split(" ",4)
        if len(p)<5: continue
        try: st=float(p[0]); dur=float(p[1])
        except ValueError: continue
        rows.append((st,dur,p[2],p[3],p[4].strip()))
    print("==", h, "long syscalls", len(rows))
    for kind,a,b,d in slow:
        hits=[r for r in rows if r[0]>=a-0.05 and r[0]<=b and r[0]+r[1]<=b+0.05]
        print("  %s %.3f..%.3f (%.3f s): %d server waits inside"%(kind,a,b,d,len(hits)))
        for r in hits[:8]:
            print("     start %.3f dur %.3f tid %s %s | %s"%(r[0],r[1],r[2],r[3],r[4][:110]))
PY
echo DONE
