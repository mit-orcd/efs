#!/bin/bash
# w125_mac_pressure.sh CFG OUT [FILES]
#
# Space-pressure check for the packer on the small Mac VM cluster
# (w125_mac_cluster.sh: n1 efs2, n2/n3 efs3, TCP, ports 33433-33435).
# Fresh cluster, quota 1G per daemon, minimum age 10 s; a 16-worker
# w125_p0_capture.py writer (<= 512 B files) on efs2 writes FILES files
# (default 24,000: unpacked, ~64 KiB per file per node, ~1.5 GiB, 150% of
# the quota). The VMs are a chaos environment: a VM reset (changed boot
# time on efs2/efs3) aborts the run with exit 3 and a CHAOS line. Every 15 s: node 1 usage, per-file fragments, segments and
# the packer's pressure/rate. CFG:
#   pressure  EFS_PACK_UNPACKED_GIB=1 (defaults otherwise: quota 50-80%)
#   floor     no space trigger (EFS_PACK_UNPACKED_GIB=100000,
#             EFS_PACK_SPACE_START_PCT=100, EFS_PACK_SPACE_FULL_PCT=100)
# Expected: pressure keeps usage under the quota and the writer completes;
# floor runs into quota refusals while the writer is busy.
# These VMs write only ~60 files/s, which the default 50 files/s busy floor
# nearly keeps up with; the floor is set to 1 file/s so the writer outruns
# it, as a fast writer outruns 50 files/s on real hardware.
set -u
CFG=$1; OUT=$2; FILES=${3:-24000}
HERE=$(cd "$(dirname "$0")" && pwd); D=/data1/w125/run
mkdir -p "$OUT"
case $CFG in
  pressure) export EFS_PACK_UNPACKED_GIB=1 ;;
  floor) export EFS_PACK_UNPACKED_GIB=100000 EFS_PACK_SPACE_START_PCT=100 EFS_PACK_SPACE_FULL_PCT=100 ;;
  *) echo "CFG: pressure|floor"; exit 2 ;;
esac
export EFS_PACK_MIN_AGE_MS=10000 EFS_PACK_BUSY_FILES_PER_S=1 QUOTA=1G
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
for h in efs2 efs3; do ssh $h "rm -rf $D"; done
"$HERE/w125_mac_cluster.sh" start "$D" > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
scp -q "$HERE/w125_p0_capture.py" efs2:/data1/w125/
ssh efs2 "cd /data1/w125 && setsid -f bash -c 'python3 w125_p0_capture.py $D/mnt/w --workers 16 --max-files $FILES --out /data1/w125/w.csv > /data1/w125/w.txt 2>&1; echo WDONE >> /data1/w125/w.txt' < /dev/null > /dev/null 2>&1"
t0=$(date +%s)
boots() { for h in efs2 efs3; do ssh -o ConnectTimeout=5 $h 'uptime -s' 2>/dev/null; done | tr '\n' ' '; }
B0=$(boots)
chaos() { local b; b=$(boots); [ "$b" = "$B0" ] && return 1; echo "CHAOS reset at $(( $(date +%s) - t0 ))s (boots $B0 -> $b)" | tee -a "$OUT/samples.txt"; return 0; }
sample() {
  local u
  u=$(ssh efs2 "cd /data1/w125/src && ./efs-mgmt status 192.168.2.12:33433 2>/dev/null | grep 'node 1:' | grep -o 'used=[^q]*'")
  echo "$(( $(date +%s) - t0 ))s $u $(ssh efs2 "echo frags=\$(find $D/n1/data/exports -type f 2>/dev/null | wc -l) segs=\$(find $D/n1/data/segments -type f 2>/dev/null | wc -l); grep -h 'packer: group' $D/logs/efsd-n1.log | tail -n 1 | grep -o 'busy=[0-9]* fg_commits_per_s=[0-9]*\|pressure=[0-9.]* unpacked_gib=[0-9.]* used_pct=[0-9.]*\|rate=[0-9]*' | tr '\n' ' '")"
}
while ! ssh -o ConnectTimeout=5 efs2 "grep -q WDONE /data1/w125/w.txt"; do
  chaos && exit 3
  sample >> "$OUT/samples.txt"; sleep 15
done
sample >> "$OUT/samples.txt"
ssh efs2 "cat /data1/w125/w.txt" > "$OUT/writer.txt"
for t in $(seq 60); do chaos && exit 3; sample >> "$OUT/samples.txt"; ssh efs2 "[ \$(find $D/n1/data/exports -type f | wc -l) -eq 0 ]" && break; sleep 10; done
ssh efs2 "grep -ac 'EDQUOT\|ENOSPC\|quota\|No space' $D/logs/fuse.log; grep -h 'packer:' $D/logs/efsd-n*.log" > "$OUT/packer.log" 2>&1
ssh efs3 "grep -h 'packer:' $D/logs/efsd-n*.log" >> "$OUT/packer.log" 2>&1
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
tail -n 3 "$OUT/writer.txt"; tail -n 2 "$OUT/samples.txt"
