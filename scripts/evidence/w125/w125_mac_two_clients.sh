#!/bin/bash
# w125_mac_two_clients.sh OUT [SECONDS]
#
# Two-client concurrent load with compaction on the small Mac VM cluster
# (w125_mac_cluster.sh: n1 efs2, n2/n3 efs3, TCP; mount A on efs2, a second
# efs-fuse mount B on efs3). Fresh cluster, quota 6G per daemon, packer
# minimum age 10 s, defaults otherwise. For SECONDS (default 3600):
#   A (efs2): w125_concurrent.py, 4 writers (atomic publish by rename),
#             4 verifying readers, 2 deleters;
#   B (efs3): w125_concurrent.py --remote, discovers A's files by listing,
#             4 verifying readers, 2 deleters (names logged).
# Every 60 s: efs-mgmt pack-compact (64 segments per led group), node 1
# usage, fragmentation summary. Afterwards: both clients' final verifies;
# every file A finds missing must have been deleted by B, and nothing B
# deleted may still exist; then everything is deleted and node 1 must
# return to no data and no segment files. A VM reset aborts (exit 3).
set -u
OUT=$1; RUN=${2:-3600}
HERE=$(cd "$(dirname "$0")" && pwd); D=/data1/w125/run; W=/data1/w125/conc
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=10000 QUOTA=6G
ssh efs3 "/data1/w125/src/scripts/client.sh stop $D/mntb > /dev/null 2>&1"
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
for h in efs2 efs3; do ssh $h "rm -rf $D/n* $D/logs $W; mkdir -p $W"; done
"$HERE/w125_mac_cluster.sh" start "$D" > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
ssh efs3 "mkdir -p $D/mntb && cd /data1/w125/src && EFS_FUSE_LOG=$D/logs/fuse-b.log scripts/client.sh 192.168.2.12:33433 $D/mntb > $D/logs/client-b.out 2>&1; findmnt -rn $D/mntb > /dev/null" || { echo "mount B failed"; exit 1; }
for h in efs2 efs3; do scp -q "$HERE/w125_concurrent.py" $h:$W/; done
boots() { for h in efs2 efs3; do ssh -o ConnectTimeout=5 $h 'uptime -s' 2>/dev/null; done | tr '\n' ' '; }
B0=$(boots)
usage() { ssh efs2 "echo n1 data=\$(du -sm $D/n1/data | cut -f1)MB segs=\$(find $D/n1/data/segments -type f | wc -l) frags=\$(find $D/n1/data/exports -type f | wc -l)"; }
ssh efs2 "cd $W && setsid -f bash -c 'python3 w125_concurrent.py $D/mnt/c $RUN > $W/a.txt 2>&1; echo \"RC=\$?\" >> $W/a.txt' < /dev/null > /dev/null 2>&1"
sleep 20
ssh efs3 "cd $W && setsid -f bash -c 'python3 w125_concurrent.py $D/mntb/c $((RUN - 20)) --remote --deleted-out $W/b.gone > $W/b.txt 2>&1; echo \"RC=\$?\" >> $W/b.txt' < /dev/null > /dev/null 2>&1"
t0=$(date +%s)
while ! { ssh efs2 "grep -q '^RC=' $W/a.txt" && ssh efs3 "grep -q '^RC=' $W/b.txt"; } 2>/dev/null; do
  sleep 60
  [ "$(boots)" != "$B0" ] && { echo "CHAOS reset at $(( $(date +%s) - t0 ))s ($B0 -> $(boots))" | tee -a "$OUT/log.txt"; exit 3; }
  { echo "t=$(( $(date +%s) - t0 ))s A: $(ssh efs2 "tail -n 1 $W/a.txt")"
    echo "   B: $(ssh efs3 "tail -n 1 $W/b.txt")"
    echo "   $(usage)"
    ssh efs2 "/data1/w125/src/efs-mgmt pack-compact 192.168.2.12:33433 64 2>&1 | grep -o 'node [0-9].*leads.*' | sed 's/^/   /'"
  } >> "$OUT/log.txt" 2>&1
done
ssh efs2 "cat $W/a.txt" > "$OUT/a.txt"; ssh efs3 "cat $W/b.txt" > "$OUT/b.txt"
ssh efs3 "cat $W/b.gone" | ssh efs2 "cat > $W/b.gone"
ssh efs2 "python3 - $D/mnt/c $W/b.gone" > "$OUT/crosscheck.txt" 2>&1 <<'EOF'
import os, sys
root, gonef = sys.argv[1], sys.argv[2]
miss = set(filter(None, open(root + ".missing").read().split("\n")))
gone = set(filter(None, open(gonef).read().split("\n")))
present = [g for g in gone if os.path.exists(os.path.join(root, g))]
print("A_missing=%d B_deleted=%d missing_not_deleted_by_B=%d B_deleted_still_present=%d" % (
    len(miss), len(gone), len(miss - gone), len(present)))
for n in sorted(miss - gone)[:10]: print("UNEXPLAINED", n)
for n in present[:10]: print("RESURRECTED", n)
sys.exit(1 if miss - gone or present else 0)
EOF
echo "crosscheck rc=$?" >> "$OUT/crosscheck.txt"
{ echo "== delete everything"
  ssh efs2 "rm -rf $D/mnt/c $D/mnt/c.live $D/mnt/c.missing $D/mnt/c.remote.live $D/mnt/c.remote.missing 2>/dev/null; ls -A $D/mnt"
  for t in $(seq 12); do sleep 15; u=$(usage); echo "t=$((t * 15))s $u"; echo "$u" | grep -q "segs=0 frags=0" && break; done
} >> "$OUT/log.txt" 2>&1
for h in efs2 efs3; do ssh $h "grep -h 'packer: compact\|packer: segments' $D/logs/efsd-n*.log"; done > "$OUT/packer.log"
ssh efs3 "/data1/w125/src/scripts/client.sh stop $D/mntb > /dev/null 2>&1"
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
tail -n 2 "$OUT/a.txt" "$OUT/b.txt"; cat "$OUT/crosscheck.txt"; tail -n 2 "$OUT/log.txt"
grep -q '^RC=0' "$OUT/a.txt" && grep -q '^RC=0' "$OUT/b.txt" && grep -q 'crosscheck rc=0' "$OUT/crosscheck.txt"
