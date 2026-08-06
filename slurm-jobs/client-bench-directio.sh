#!/bin/bash
#SBATCH --job-name=efs-dio-cli
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/dio-cli-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/dio-cli-%j.err
#
# Client-side A/B workload against a 3-node IB cluster.
# Env: MODE=buffered|direct  OUT_DIR=<shared results dir>

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
MODE="${MODE:-direct}"
OUT_DIR="${OUT_DIR:-$SHARED/profile/dio-cluster-pending}"
SEQ_MB="${SEQ_MB:-64}"
NSEQ="${NSEQ:-2}"
SMALL_N="${SMALL_N:-64}"
SMALL_KB="${SMALL_KB:-64}"

SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$SCRATCH/mnt"
FUSE_PID=""

cleanup() {
    set +e
    if [ -n "${MNT:-}" ]; then
        fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    fi
    if [ -n "${FUSE_PID:-}" ]; then
        kill -TERM "$FUSE_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$FUSE_PID" 2>/dev/null || true
        wait "$FUSE_PID" 2>/dev/null || true
    fi
    echo "cleaning /scratch: $SCRATCH"
    rm -rf "$SCRATCH" 2>/dev/null || true
}
trap cleanup EXIT

rm -rf "$SCRATCH"
mkdir -p "$MNT" "$OUT_DIR" "$SHARED/logs"

secs() { date +%s.%N; }
elapsed() { awk -v s="$1" -v e="$2" 'BEGIN { printf "%.3f", e - s }'; }
mibs() {
    awk -v m="$1" -v s="$2" 'BEGIN {
        if (s <= 0) { print "inf"; exit }
        printf "%.2f", m / s
    }'
}

echo "dio-cli MODE=$MODE host=$(hostname -s) job=$SLURM_JOB_ID OUT=$OUT_DIR"
echo "params: SEQ_MB=$SEQ_MB NSEQ=$NSEQ SMALL_N=$SMALL_N SMALL_KB=$SMALL_KB"

for i in 1 2 3; do
    while [ ! -s "$SHARED/state/s${i}.addr" ]; do sleep 2; done
done
S1=$(cat "$SHARED/state/s1.addr")
S2=$(cat "$SHARED/state/s2.addr")
S3=$(cat "$SHARED/state/s3.addr")
echo "servers (IB): $S1 $S2 $S3"

for addr in "$S1" "$S2" "$S3"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 300 ]; then
            echo "Timed out waiting for $addr"
            exit 1
        fi
    done
    echo "  reachable $addr"
done

EXPORT="dio-${MODE}"
echo "mkfs $EXPORT via $S1 ..."
MKFS_OK=0
for attempt in 1 2 3 4 5; do
    if "$REPO/efs-mgmt" mkfs "$S1" "$EXPORT"; then
        MKFS_OK=1
        break
    fi
    echo "mkfs attempt $attempt failed; retrying..."
    sleep 2
done
[ "$MKFS_OK" = 1 ] || { echo "mkfs failed"; exit 1; }
# Brief settle so peers replicate the new export before FUSE mounts.
sleep 2

echo "mounting fuse..."
"$REPO/efs-fuse" "$S1" "$S2" "$S3" "$EXPORT" "$MNT" -f \
    > "$OUT_DIR/${MODE}-fuse.log" 2>&1 &
FUSE_PID=$!
for i in $(seq 1 60); do
    if mountpoint -q "$MNT" 2>/dev/null; then break; fi
    if ! kill -0 "$FUSE_PID" 2>/dev/null; then
        echo "fuse died:"; cat "$OUT_DIR/${MODE}-fuse.log"; exit 1
    fi
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$OUT_DIR/${MODE}-fuse.log"; exit 1; }
echo "mounted on $(hostname -s):$MNT"

{
    echo "client_host=$(hostname -s)"
    echo "client_job=$SLURM_JOB_ID"
    echo "mode=$MODE"
    echo "servers=$S1 $S2 $S3"
    for i in 1 2 3; do
        echo "s${i}_node=$(cat "$SHARED/state/s${i}.node" 2>/dev/null || true)"
    done
} | tee "$OUT_DIR/${MODE}-topo.txt"

t0=$(secs)
for i in $(seq 1 "$NSEQ"); do
    dd if=/dev/zero of="$MNT/seq-${i}.bin" bs=1M count="$SEQ_MB" status=none conv=fsync
done
sync "$MNT"
t1=$(secs)
dt=$(elapsed "$t0" "$t1")
total_mb=$((NSEQ * SEQ_MB))
line="SEQ_WRITE mode=$MODE bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
echo "$line"
echo "$line" >> "$OUT_DIR/results.txt"

if [ -w /proc/sys/vm/drop_caches ]; then
    echo 3 > /proc/sys/vm/drop_caches || true
fi
t0=$(secs)
for i in $(seq 1 "$NSEQ"); do
    dd if="$MNT/seq-${i}.bin" of=/dev/null bs=1M status=none
done
t1=$(secs)
dt=$(elapsed "$t0" "$t1")
line="SEQ_READ  mode=$MODE bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
echo "$line"
echo "$line" >> "$OUT_DIR/results.txt"

mkdir -p "$MNT/small"
t0=$(secs)
ok=0
fail=0
set +e
for i in $(seq 1 "$SMALL_N"); do
    if dd if=/dev/urandom of="$MNT/small/f-${i}.bin" bs=1K count="$SMALL_KB" status=none conv=fsync; then
        ok=$((ok + 1))
    else
        fail=$((fail + 1))
        echo "SMALL_WR file $i failed" >&2
    fi
done
sync "$MNT/small" 2>/dev/null || true
set -e
t1=$(secs)
dt=$(elapsed "$t0" "$t1")
total_mb=$(awk -v n="$ok" -v k="$SMALL_KB" 'BEGIN { printf "%.3f", n * k / 1024 }')
line="SMALL_WR  mode=$MODE files_ok=$ok files_fail=$fail each_kiB=$SMALL_KB bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
echo "$line"
echo "$line" >> "$OUT_DIR/results.txt"

echo "MODE=$MODE DONE"
