#!/bin/bash
#SBATCH --job-name=efs-join4v
#SBATCH --partition=mit_normal
#SBATCH --time=00:20:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/join4v-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/join4v-%j.err
#
# After mid-load join of s4: remount via s1 (LIST_NODES → expect 4), try to
# read a file written under 3-node membership, then write a new 64 MiB file and
# check whether s4 used increases (placement under node_count=4).

set -euo pipefail
REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PROF_ROOT="${PROF_ROOT:?}"
EXPORT_NAME="${EXPORT_NAME:-join4load}"

LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$LOCAL/mnt"
rm -rf "$LOCAL"
mkdir -p "$MNT" "$PROF_ROOT"

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    kill -KILL ${CPID:-} 2>/dev/null || true
    rm -rf "$LOCAL"
}
trap cleanup EXIT

S1=$(cat "$SHARED/state/s1.addr")
echo "verify on $(hostname -s) via $S1 export=$EXPORT_NAME"

echo "=== membership before mount ==="
"$REPO/efs-mgmt" status "$S1" | tee "$PROF_ROOT/verify-status-pre.txt"

"$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f >"$PROF_ROOT/verify-fuse.log" 2>&1 &
CPID=$!
for _ in $(seq 1 60); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "fuse died:"; cat "$PROF_ROOT/verify-fuse.log"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF_ROOT/verify-fuse.log"; exit 1; }

# Find a file written during the 3-node load
OLD=$(find "$MNT" -type f -name 'stream-*.bin' 2>/dev/null | head -1 || true)
echo "old_file=${OLD:-NONE}" | tee "$PROF_ROOT/verify-read.txt"
if [ -n "$OLD" ]; then
    set +e
    timeout 60 dd if="$OLD" of=/dev/null bs=1M count=16 status=none 2>"$PROF_ROOT/verify-read.err"
    rc=$?
    set -e
    echo "old_read_rc=$rc" | tee -a "$PROF_ROOT/verify-read.txt"
    cat "$PROF_ROOT/verify-read.err" >> "$PROF_ROOT/verify-read.txt" || true
else
    echo "old_read_rc=missing" | tee -a "$PROF_ROOT/verify-read.txt"
fi

echo "=== write 64 MiB under post-join membership ==="
NEW="$MNT/postjoin-64m.bin"
start=$(date +%s.%N)
dd if=/dev/zero of="$NEW" bs=1M count=64 status=none
sync
end=$(date +%s.%N)
echo "new_write_wall=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')" | tee "$PROF_ROOT/verify-write.txt"
stat -c 'new_bytes=%s' "$NEW" | tee -a "$PROF_ROOT/verify-write.txt"

# Read it back
set +e
timeout 60 dd if="$NEW" of=/dev/null bs=1M count=64 status=none
echo "new_read_rc=$?" | tee -a "$PROF_ROOT/verify-write.txt"
set -e

fusermount -u "$MNT" 2>/dev/null || true
kill -TERM "$CPID" 2>/dev/null || true
sleep 1
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

echo "=== membership after verify write ==="
"$REPO/efs-mgmt" status "$S1" | tee "$PROF_ROOT/verify-status-post.txt"
echo "VERIFY_DONE"
