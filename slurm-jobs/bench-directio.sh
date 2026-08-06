#!/bin/bash
#SBATCH --job-name=efs-dio-bench
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/dio-bench-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/dio-bench-%j.err
#
# Single-node A/B: buffered (--no-direct-io) vs default O_DIRECT.
# Writes/reads a few sequential files under /scratch; prints MiB/s.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/profile/dio-bench-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
IP="127.0.0.1"
P1=1991
P2=1992
P3=1993
# Modest sizes: enough to see a difference, not a load test.
SEQ_MB=${SEQ_MB:-64}
NSEQ=${NSEQ:-2}
SMALL_N=${SMALL_N:-64}
SMALL_KB=${SMALL_KB:-64}

mkdir -p "$OUT" "$SHARED/logs"

MNT=""
cleanup() {
    set +e
    if [ -n "${MNT:-}" ]; then
        fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    fi
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

secs() {
    # portable monotonic-ish wall time in seconds with ms via date
    date +%s.%N
}

elapsed() {
    local start=$1 end=$2
    awk -v s="$start" -v e="$end" 'BEGIN { printf "%.3f", e - s }'
}

mibs() {
    local mb=$1 sec=$2
    awk -v m="$mb" -v s="$sec" 'BEGIN {
        if (s <= 0) { print "inf"; exit }
        printf "%.2f", m / s
    }'
}

run_mode() {
    local mode=$1   # buffered | direct
    local dio_flag
    if [ "$mode" = "buffered" ]; then
        dio_flag="--no-direct-io"
    else
        dio_flag="--direct-io"
    fi

    MNT="$LOCAL/$mode/mnt"
    rm -rf "$LOCAL/$mode"
    mkdir -p "$LOCAL/$mode/s1" "$LOCAL/$mode/s2" "$LOCAL/$mode/s3" "$MNT"

    for port in "$P1" "$P2" "$P3"; do
        fuser -k "${port}/tcp" 2>/dev/null || true
    done
    sleep 1

    echo "=== mode=$mode flag=$dio_flag ==="

    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/$mode/s1" \
        "$dio_flag" > "$OUT/${mode}-s1.log" 2>&1 &
    S1=$!
    for i in $(seq 1 40); do
        if grep -q "listening on" "$OUT/${mode}-s1.log" 2>/dev/null; then break; fi
        if ! kill -0 "$S1" 2>/dev/null; then
            echo "s1 died:"; cat "$OUT/${mode}-s1.log"; return 1
        fi
        sleep 0.25
    done
    grep -q "listening on" "$OUT/${mode}-s1.log" || { echo "s1 not ready"; cat "$OUT/${mode}-s1.log"; return 1; }
    grep "direct_io=" "$OUT/${mode}-s1.log" || true

    "$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/$mode/s2" \
        --join "$IP:$P1" "$dio_flag" > "$OUT/${mode}-s2.log" 2>&1 &
    S2=$!
    "$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/$mode/s3" \
        --join "$IP:$P1" "$dio_flag" > "$OUT/${mode}-s3.log" 2>&1 &
    S3=$!

    for port in "$P2" "$P3"; do
        READY=0
        for i in $(seq 1 40); do
            if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then READY=1; break; fi
            sleep 0.25
        done
        [ "$READY" = "1" ] || { echo "server :$port failed"; cat "$OUT/${mode}-s"*.log; return 1; }
    done
    # Wait until s2/s3 have actually joined (not just listening).
    for log in "$OUT/${mode}-s2.log" "$OUT/${mode}-s3.log"; do
        for i in $(seq 1 40); do
            if grep -q "Joined cluster" "$log" 2>/dev/null; then break; fi
            sleep 0.25
        done
        grep -q "Joined cluster" "$log" || { echo "join failed: $log"; cat "$log"; return 1; }
    done
    sleep 1

    echo "mkfs..."
    MKFS_OK=0
    for attempt in 1 2 3 4 5; do
        if "$REPO/efs-mgmt" mkfs "$IP:$P1" "dio-${mode}"; then
            MKFS_OK=1
            break
        fi
        echo "mkfs attempt $attempt failed; retrying..."
        sleep 1
    done
    if [ "$MKFS_OK" != 1 ]; then
        echo "mkfs failed"; cat "$OUT/${mode}-s"*.log; return 1
    fi

    echo "mounting fuse..."
    "$REPO/efs-fuse" "$IP:$P1" "dio-${mode}" "$MNT" -f \
        > "$OUT/${mode}-fuse.log" 2>&1 &
    CPID=$!
    for i in $(seq 1 40); do
        if mountpoint -q "$MNT" 2>/dev/null; then break; fi
        if ! kill -0 "$CPID" 2>/dev/null; then
            echo "fuse died:"; cat "$OUT/${mode}-fuse.log"; return 1
        fi
        sleep 0.25
    done
    if ! mountpoint -q "$MNT"; then
        echo "mount failed"; cat "$OUT/${mode}-fuse.log"; return 1
    fi
    echo "mounted"

    local t0 t1 dt total_mb

    # Sequential large writes
    t0=$(secs)
    for i in $(seq 1 "$NSEQ"); do
        dd if=/dev/zero of="$MNT/seq-${i}.bin" bs=1M count="$SEQ_MB" status=none conv=fsync
    done
    sync "$MNT"
    t1=$(secs)
    dt=$(elapsed "$t0" "$t1")
    total_mb=$((NSEQ * SEQ_MB))
    echo "SEQ_WRITE mode=$mode bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
    echo "SEQ_WRITE mode=$mode bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")" >> "$OUT/results.txt"

    # Sequential large reads (drop page cache only when permitted)
    if [ -w /proc/sys/vm/drop_caches ]; then
        echo 3 > /proc/sys/vm/drop_caches || true
    fi
    t0=$(secs)
    for i in $(seq 1 "$NSEQ"); do
        dd if="$MNT/seq-${i}.bin" of=/dev/null bs=1M status=none
    done
    t1=$(secs)
    dt=$(elapsed "$t0" "$t1")
    echo "SEQ_READ  mode=$mode bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
    echo "SEQ_READ  mode=$mode bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")" >> "$OUT/results.txt"

    # Many small files
    mkdir -p "$MNT/small"
    t0=$(secs)
    for i in $(seq 1 "$SMALL_N"); do
        dd if=/dev/urandom of="$MNT/small/f-${i}.bin" bs=1K count="$SMALL_KB" status=none conv=fsync
    done
    sync "$MNT/small"
    t1=$(secs)
    dt=$(elapsed "$t0" "$t1")
    total_mb=$(awk -v n="$SMALL_N" -v k="$SMALL_KB" 'BEGIN { printf "%.3f", n * k / 1024 }')
    echo "SMALL_WR  mode=$mode files=$SMALL_N each_kiB=$SMALL_KB bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")"
    echo "SMALL_WR  mode=$mode files=$SMALL_N each_kiB=$SMALL_KB bytes_miB=$total_mb seconds=$dt mib_s=$(mibs "$total_mb" "$dt")" >> "$OUT/results.txt"

    # Tear down this mode before the next (KILL avoids efsd SIGINT shutdown crashes).
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in "$CPID" "$S1" "$S2" "$S3"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in "$CPID" "$S1" "$S2" "$S3"; do
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    CPID=""; S1=""; S2=""; S3=""
    for port in "$P1" "$P2" "$P3"; do
        fuser -k "${port}/tcp" 2>/dev/null || true
    done
    sleep 1
}

echo "bench on $(hostname) job=$SLURM_JOB_ID OUT=$OUT"
echo "params: SEQ_MB=$SEQ_MB NSEQ=$NSEQ SMALL_N=$SMALL_N SMALL_KB=$SMALL_KB" | tee "$OUT/results.txt"

# Build first if binaries look stale / missing (compute node).
if [ ! -x "$REPO/efsd" ] || [ ! -x "$REPO/efs-fuse" ]; then
    echo "binaries missing; build required before this job"
    exit 1
fi

# buffered first, then default direct-io
run_mode buffered
run_mode direct

echo
echo "=== SUMMARY ==="
cat "$OUT/results.txt"
echo "Results: $OUT/results.txt"
