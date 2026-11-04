#!/bin/bash
# w125_mac_concurrent.sh OUT [SECONDS]
#
# Concurrent load with compaction on the small Mac VM cluster
# (w125_mac_cluster.sh: n1 efs2, n2/n3 efs3, TCP): fresh cluster, quota 6G
# per daemon, packer minimum age 10 s, defaults otherwise.
# w125_concurrent.py runs on efs2 for SECONDS (default 1200): 4 writers,
# 4 verifying readers, 2 deleters, file sizes log-uniform 1 B - 4 MiB.
# Every 60 s during the load: efs-mgmt pack-compact (64 segments per led
# group), node 1 usage and the fragmentation summary. Afterwards the tool
# verifies every remaining file; then everything is deleted and node 1
# must return to no data and no segment files. A VM reset (chaos
# environment) aborts with exit 3.
set -u
OUT=$1; SECONDS_RUN=${2:-1200}
HERE=$(cd "$(dirname "$0")" && pwd); D=/data1/w125/run; W=/data1/w125/conc
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=10000 QUOTA=6G
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
for h in efs2 efs3; do ssh $h "rm -rf $D/n* $D/logs $W"; done
"$HERE/w125_mac_cluster.sh" start "$D" > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
ssh efs2 "mkdir -p $W"; scp -q "$HERE/w125_concurrent.py" efs2:$W/
boots() { for h in efs2 efs3; do ssh -o ConnectTimeout=5 $h 'uptime -s' 2>/dev/null; done | tr '\n' ' '; }
B0=$(boots)
usage() { ssh efs2 "echo n1 data=\$(du -sm $D/n1/data | cut -f1)MB segs=\$(find $D/n1/data/segments -type f | wc -l) frags=\$(find $D/n1/data/exports -type f | wc -l)"; }
ssh efs2 "cd $W && setsid -f bash -c 'python3 w125_concurrent.py $D/mnt/c $SECONDS_RUN > $W/run.txt 2>&1; echo \"RC=\$?\" >> $W/run.txt' < /dev/null > /dev/null 2>&1"
t0=$(date +%s)
while ! ssh efs2 "grep -q '^RC=' $W/run.txt" 2>/dev/null; do
  sleep 60
  [ "$(boots)" != "$B0" ] && { echo "CHAOS reset at $(( $(date +%s) - t0 ))s ($B0 -> $(boots))" | tee -a "$OUT/log.txt"; exit 3; }
  { echo "t=$(( $(date +%s) - t0 ))s $(ssh efs2 "tail -n 1 $W/run.txt")"
    echo "   $(usage)"
    ssh efs2 "/data1/w125/src/efs-mgmt pack-compact 192.168.2.12:33433 64 2>&1 | grep -o 'node [0-9].*leads.*' | sed 's/^/   /'"
  } >> "$OUT/log.txt" 2>&1
done
ssh efs2 "cat $W/run.txt" > "$OUT/run.txt"
{ echo "== delete everything"
  ssh efs2 "rm -rf $D/mnt/c $D/mnt/c.live"
  for t in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 15; u=$(usage); echo "t=$((t * 15))s $u"; echo "$u" | grep -q "segs=0 frags=0" && break; done
} >> "$OUT/log.txt" 2>&1
for h in efs2 efs3; do ssh $h "grep -h 'packer: compact\|packer: segments' $D/logs/efsd-n*.log"; done > "$OUT/packer.log"
ssh efs2 "grep -aiE 'error|fail|stale|EIO' $D/logs/fuse.log | grep -v 'fail=-/0' | tail -n 20" > "$OUT/fuse-errors.txt"
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
tail -n 4 "$OUT/run.txt"; tail -n 2 "$OUT/log.txt"
grep -q '^RC=0' "$OUT/run.txt"
