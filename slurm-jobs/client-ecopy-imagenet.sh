#!/bin/bash
#SBATCH --job-name=efs-ecopy
#SBATCH --partition=mit_normal
#SBATCH --time=04:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=32G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/ecopy-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/ecopy-%j.err
#
# Mount efs-test and ecopy SRC → $MNT/imagenet.
# Env: EXPORT_NAME, SRC, PROF_ROOT, NUM_SERVERS

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ECOPY="${ECOPY:-/home/erbmi1/git/direct_copy/ecopy}"
EXPORT_NAME="${EXPORT_NAME:-efs-imagenet}"
SRC="${SRC:-/home/erbmi1/orcd/scratch/imagenet}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_ROOT="${PROF_ROOT:-$SHARED/logs/ecopy-last}"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$SCRATCH/mnt"
rm -rf "$SCRATCH"
mkdir -p "$MNT" "$PROF_ROOT" "$SHARED/logs"

FUSE_PID=""
cleanup() {
    set +e
    echo "=== client cleanup on $(hostname) ==="
    if mountpoint -q "$MNT" 2>/dev/null || grep -q " $MNT " /proc/mounts 2>/dev/null; then
        fusermount -uz "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    fi
    if [ -n "${FUSE_PID:-}" ]; then
        kill -TERM "$FUSE_PID" 2>/dev/null || true
        for _ in $(seq 1 40); do
            kill -0 "$FUSE_PID" 2>/dev/null || break
            sleep 0.25
        done
        kill -KILL "$FUSE_PID" 2>/dev/null || true
        wait "$FUSE_PID" 2>/dev/null || true
    fi
    echo "cleaning /scratch: $SCRATCH"
    rm -rf "$SCRATCH"
}
trap cleanup EXIT TERM INT

echo "ecopy client on $(hostname -s) SRC=$SRC EXPORT=$EXPORT_NAME"
efs_assert_distinct_servers "$NUM_SERVERS"

NODES=()
for i in $(seq 1 "$NUM_SERVERS"); do
    NODES+=("$(cat "$SHARED/state/s${i}.addr")")
done
echo "servers: ${NODES[*]}"

for addr in "${NODES[@]}"; do
    host=${addr%:*}
    port=${addr#*:}
    ok=0
    for _ in $(seq 1 60); do
        if timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; then ok=1; break; fi
        sleep 2
    done
    [ "$ok" = 1 ] || { echo "ERROR: $addr not reachable"; exit 1; }
done

export FUSE_THREAD_STACK="${FUSE_THREAD_STACK:-8388608}"
export EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}"

FUSE_LOG="$PROF_ROOT/fuse-${SLURM_JOB_ID}.log"
echo "starting efs-fuse → $FUSE_LOG"
"$REPO/efs-fuse" "${NODES[@]}" "$EXPORT_NAME" "$MNT" -f \
    > "$FUSE_LOG" 2>&1 &
FUSE_PID=$!

MOUNTED=0
for i in $(seq 1 120); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$FUSE_PID" 2>/dev/null; then
        echo "ERROR: efs-fuse died during mount"
        cat "$FUSE_LOG" || true
        exit 1
    fi
    sleep 0.5
done
[ "$MOUNTED" = 1 ] || { echo "ERROR: mount timeout"; cat "$FUSE_LOG"; exit 1; }
echo "mounted OK"
head -30 "$FUSE_LOG" || true

# Quick smoke before the big copy
echo "smoke: mkdir + 1MB write"
mkdir -p "$MNT/smoke"
dd if=/dev/urandom of="$MNT/smoke/t.bin" bs=1M count=1 status=none
sync
SIZE=$(stat -c%s "$MNT/smoke/t.bin")
[ "$SIZE" = "1048576" ] || { echo "ERROR: smoke write size=$SIZE"; exit 1; }
echo "smoke OK"

DST="$MNT/imagenet"
mkdir -p "$DST"
echo "=== ecopy $SRC → $DST ==="
date -Is
ECOPY_LOG="$PROF_ROOT/ecopy-${SLURM_JOB_ID}.log"
set +e
timeout --signal=TERM 10800 "$ECOPY" "$SRC" "$DST" 2>&1 | tee "$ECOPY_LOG"
RC=${PIPESTATUS[0]}
set -e
date -Is
echo "ecopy rc=$RC"

if ! kill -0 "$FUSE_PID" 2>/dev/null; then
    echo "ERROR: efs-fuse died during ecopy"
    echo "--- fuse log tail ---"
    tail -100 "$FUSE_LOG" || true
    echo "FAIL fuse_dead" > "$PROF_ROOT/RESULT"
    exit 1
fi

if [ "$RC" -ne 0 ]; then
    echo "ERROR: ecopy failed rc=$RC"
    echo "--- ecopy log tail ---"
    tail -80 "$ECOPY_LOG" || true
    echo "--- fuse log tail ---"
    tail -80 "$FUSE_LOG" || true
    echo "FAIL ecopy_$RC" > "$PROF_ROOT/RESULT"
    exit 1
fi

# Spot-check: at least some files landed
N=$(find "$DST" -type f 2>/dev/null | wc -l || echo 0)
echo "dst file count=$N"
if [ "$N" -lt 100 ]; then
    echo "ERROR: too few files copied ($N)"
    echo "FAIL few_files" > "$PROF_ROOT/RESULT"
    exit 1
fi

echo "PASS ecopy files=$N" | tee "$PROF_ROOT/RESULT"
exit 0
