#!/bin/bash
# orcd_mini_two_clients.sh OUT [SECONDS]
#
# Two-client concurrent load with compaction on orcd-mini, driven from the
# Mac (no SSH between the nodes): client A = node9901 mount /data1/efs/mnt,
# client B = node9900 mount /data1/efs/orcd-mini-mnt, same file system.
# For SECONDS (default 3600):
#   A: w125_concurrent.py (4 writers with atomic publish, 4 verifying
#      readers, 2 deleters) under /c-<tag>;
#   B: w125_concurrent.py --remote on the same directory (discovers, verifies,
#      deletes, logs deletions).
# Every 60 s: efs-mgmt pack-compact (64 segments per led group) and the
# fragmentation summary. Afterwards both final verifies and the cross-check
# (A's missing files must be B's deletions; none may reappear), then the
# test directory is removed. Raw output stays on the nodes under
# /data1/efs/prof/two-<tag> and /data1/efs/orcd-mini-client/prof/two-<tag>.
set -u
OUT=$1; RUN=${2:-3600}; TAG=$(date -u +%Y%m%dT%H%M%SZ)
S=$(cd "$(dirname "$0")" && pwd)/orcd_mini_ssh.sh
A=/data1/efs/prof/two-$TAG; B=/data1/efs/orcd-mini-client/prof/two-$TAG
mkdir -p "$OUT"
$S node9901 "mkdir -p $A && cp /data1/efs/src/scripts/w125_concurrent.py $A/" 2>/dev/null
$S node9900 "mkdir -p $B && cp /data1/efs/orcd-mini-client/src/scripts/w125_concurrent.py $B/" 2>/dev/null
$S node9901 "cd $A && setsid -f bash -c 'python3 w125_concurrent.py /data1/efs/mnt/c-$TAG $RUN > $A/a.txt 2>&1; echo RC=\$? >> $A/a.txt' < /dev/null > /dev/null 2>&1" 2>/dev/null
sleep 20
$S node9900 "cd $B && setsid -f bash -c 'python3 w125_concurrent.py /data1/efs/orcd-mini-mnt/c-$TAG $((RUN - 20)) --remote --deleted-out $B/b.gone > $B/b.txt 2>&1; echo RC=\$? >> $B/b.txt' < /dev/null > /dev/null 2>&1" 2>/dev/null
echo "tag $TAG" > "$OUT/log.txt"; t0=$(date +%s)
while ! { $S node9901 "grep -q '^RC=' $A/a.txt" && $S node9900 "grep -q '^RC=' $B/b.txt"; } 2>/dev/null; do
  sleep 60
  { echo "t=$(( $(date +%s) - t0 ))s A: $($S node9901 "tail -n 1 $A/a.txt" 2>/dev/null)"
    echo "   B: $($S node9900 "tail -n 1 $B/b.txt" 2>/dev/null)"
    $S node9901 "/data1/efs/src/efs-mgmt pack-compact 10.1.99.1:17432 64 2>&1 | grep -o 'node [0-9].*leads group.*' | sed 's/^/   /'" 2>/dev/null
  } >> "$OUT/log.txt"
done
$S node9901 "cat $A/a.txt" > "$OUT/a.txt" 2>/dev/null; $S node9900 "cat $B/b.txt" > "$OUT/b.txt" 2>/dev/null
$S node9900 "cat $B/b.gone" 2>/dev/null | $S node9901 "cat > $A/b.gone" 2>/dev/null
$S node9901 "python3 - /data1/efs/mnt/c-$TAG $A/b.gone" > "$OUT/crosscheck.txt" 2>/dev/null <<'EOF'
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
$S node9901 "rm -rf /data1/efs/mnt/c-$TAG /data1/efs/mnt/c-$TAG.live /data1/efs/mnt/c-$TAG.missing /data1/efs/mnt/c-$TAG.remote.live /data1/efs/mnt/c-$TAG.remote.missing" 2>/dev/null
grep -h ERR "$OUT/a.txt" "$OUT/b.txt" | head -20; tail -n 2 "$OUT/a.txt" "$OUT/b.txt"; cat "$OUT/crosscheck.txt"
grep -q '^RC=0' "$OUT/a.txt" && grep -q '^RC=0' "$OUT/b.txt" && grep -q 'crosscheck rc=0' "$OUT/crosscheck.txt"
