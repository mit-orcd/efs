#!/bin/bash
# w125_mac_mixed.sh OUT [ROUNDS]
#
# Mixed-size create/read/delete/compact rounds on the small Mac VM cluster
# (w125_mac_cluster.sh: n1 efs2, n2/n3 efs3, TCP), with the user's testing
# scripts (~/git/cluster/testing: tiny_files.py, tiny_files_md5.py,
# random_delete.py) on efs2. Fresh cluster, quota 6G per daemon, packer
# minimum age 10 s, defaults otherwise. Each round k:
#   1. tiny_files.py MNT/big-k: 400 files, 1 B - 4 MiB (uniform);
#      tiny_files.py MNT/small-k: 2,000 files, 1 B - 16 KiB (packed);
#   2. tiny_files_md5.py over MNT (manifest A_k);
#   3. random_delete.py: half of all files;
#   4. efs-mgmt pack-compact (64 segments per led group), 60 s;
#   5. tiny_files_md5.py again (B_k): every survivor must hash as in the
#      merged baseline, with no read error (w125_md5_compare.py).
# Then everything is deleted and node 1 must return to no data and no
# segment files. The VMs are a chaos environment: a reset aborts (exit 3).
set -u
OUT=$1; ROUNDS=${2:-4}
HERE=$(cd "$(dirname "$0")" && pwd); D=/data1/w125/run; T=~/git/cluster/testing; W=/data1/w125/mixed
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=10000 QUOTA=6G
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
for h in efs2 efs3; do ssh $h "rm -rf $D/n* $D/logs; rm -rf $W"; done
"$HERE/w125_mac_cluster.sh" start "$D" > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
ssh efs2 "mkdir -p $W"; scp -q "$T/tiny_files.py" "$T/tiny_files_md5.py" "$T/random_delete.py" "$HERE/w125_md5_compare.py" efs2:$W/
boots() { for h in efs2 efs3; do ssh -o ConnectTimeout=5 $h 'uptime -s' 2>/dev/null; done | tr '\n' ' '; }
B0=$(boots); M=$D/mnt; R="ssh efs2 cd $W &&"
chaos() { [ "$(boots)" != "$B0" ] && { echo "CHAOS reset ($B0 -> $(boots))" | tee -a "$OUT/log.txt"; exit 3; }; return 0; }
summary() { ssh efs2 "/data1/w125/src/efs-mgmt pack-compact 192.168.2.12:33433 0 2>&1 | grep leads | grep -o 'segments=.*'"; }
usage() { ssh efs2 "echo n1 data=\$(du -sm $D/n1/data | cut -f1)MB segs=\$(find $D/n1/data/segments -type f | wc -l) frags=\$(find $D/n1/data/exports -type f | wc -l)"; }
FAIL=0
for k in $(seq "$ROUNDS"); do
  { echo "== round $k"
    $R "python3 tiny_files.py $M/big-$k --max-files 400 --max-file-size 4194304 --max-depth 2 --max-files-per-folder 40 --workers 8 2>&1 | tail -n 1"
    $R "python3 tiny_files.py $M/small-$k --max-files 2000 --max-file-size 16384 --max-depth 2 --max-files-per-folder 200 --workers 8 2>&1 | tail -n 1"
    sleep 20; echo "after write: $(usage) | $(summary)"
    $R "python3 tiny_files_md5.py $M --workers 16 --output $W/A$k.jsonl > /dev/null 2>&1; python3 w125_md5_compare.py $W/base.jsonl $W/A$k.jsonl --merge $W/base.jsonl 2>/dev/null || python3 w125_md5_compare.py $W/A$k.jsonl $W/A$k.jsonl --merge $W/base.jsonl"
    n=$(ssh efs2 "find $M -type f | wc -l")
    $R "python3 random_delete.py $M -n $((n / 2)) --seed $k 2>&1 | tail -n 1"
    sleep 15; echo "after delete: $(usage) | $(summary)"
    ssh efs2 "/data1/w125/src/efs-mgmt pack-compact 192.168.2.12:33433 64 2>&1 | grep -c queued"
    sleep 60; echo "after compact: $(usage) | $(summary)"
    $R "python3 tiny_files_md5.py $M --workers 16 --output $W/B$k.jsonl > /dev/null 2>&1; python3 w125_md5_compare.py $W/base.jsonl $W/B$k.jsonl" || echo "ROUND $k VERIFY FAIL"
  } >> "$OUT/log.txt" 2>&1
  grep -q "ROUND $k VERIFY FAIL" "$OUT/log.txt" && FAIL=1
  chaos
done
{ echo "== delete everything"
  ssh efs2 "rm -rf $M/big-* $M/small-*"
  for t in 1 2 3 4 5 6 7 8 9 10 11 12; do sleep 15; u=$(usage); echo "t=$((t * 15))s $u"; echo "$u" | grep -q "segs=0 frags=0" && break; done
} >> "$OUT/log.txt" 2>&1
chaos
for h in efs2 efs3; do ssh $h "grep -h 'packer: compact\|packer: segments' $D/logs/efsd-n*.log"; done > "$OUT/packer.log"
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
grep -E "^== |mismatch|FAIL|t=.*segs" "$OUT/log.txt" | tail -n 12
exit $FAIL
