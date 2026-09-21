#!/bin/bash
# On-CPU profile of the group-0 leader (fcstor003) and the dual host
# (fcstor004) while fcstor007 runs root mkdir+rmdir pairs. Confirms where
# the ~0.7 s apply stall goes (expected: efs_txn_resolve/efs_txn_drop full
# shard scans in kv merge_scan). Attaches to the RUNNING efsd; ~40 s.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-perf
preflight_or_die

SRV="fcstor003 fcstor004"
say "== perf record efsd on $SRV for 18 s"
for h in $SRV; do
    ssh_ 60 "$h" 'pid=$(pgrep -x efsd | head -1); rm -f /tmp/w8-perf.data
        perf record -e cpu-clock -F 999 -g --call-graph fp -p $pid -o /tmp/w8-perf.data sleep 18 >/dev/null 2>&1
        echo "== '"$h"' top symbols (self)"
        timeout 40 perf report --stdio --no-children -n -i /tmp/w8-perf.data 2>/dev/null | grep -E "^ +[0-9]+\.[0-9]+%" | head -25
        echo "== '"$h"' callers of merge_scan"
        timeout 40 perf report --stdio --no-children -n -i /tmp/w8-perf.data -S merge_scan -G 2>/dev/null | grep -E "efs_txn|host_|apply|lsm|merge_scan|kv_" | head -30' \
        > "$OUT/perf-$h.txt" 2>&1 &
done
sleep 2

say "== 16 root mkdir+rmdir"
ssh_ 30 fcstor007 'timeout -k 2 20 python3 - <<"PY"
import os, tempfile, time
mnt="/tmp/efs-mount"
slow=0
for i in range(16):
    t0=time.time(); d=tempfile.mkdtemp(prefix="w8p-", dir=mnt); t1=time.time()
    try: os.rmdir(d)
    except OSError as e: print("RMDIR_ERR", e)
    t2=time.time()
    if t1-t0>0.5: slow+=1
    print("mkdir %.3f rmdir %.3f"%(t1-t0,t2-t1))
    time.sleep(0.2)
print("slow", slow)
PY' | tee "$OUT/client-ops.txt"
wait
for h in $SRV; do cat "$OUT/perf-$h.txt"; done
echo DONE
