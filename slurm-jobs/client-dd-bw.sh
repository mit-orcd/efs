#!/bin/bash
#SBATCH --job-name=efs-dd-bw
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/dd-bw-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/dd-bw-%j.err
#
# Single-stream FUSE dd write+read bandwidth (+ optional store-bench + perf).
# Env: EXPORT_NAME, PROF_ROOT, NUM_SERVERS, FILE_GIB (default 4),
#      DO_STORE_BENCH=0|1, EFS_FUSE_PERF=0|1, EFS_BENCH_PERF=0|1

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
EXPORT_NAME="${EXPORT_NAME:-ddbw}"
NUM_SERVERS="${NUM_SERVERS:-3}"
FILE_GIB="${FILE_GIB:-4}"
PROF_ROOT="${PROF_ROOT:?PROF_ROOT required}"
PROF="$PROF_ROOT/client-${SLURM_JOB_ID}"
DO_STORE_BENCH="${DO_STORE_BENCH:-1}"
mkdir -p "$PROF" "$SHARED/logs"

LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
MNT=$LOCAL/mnt
rm -rf "$LOCAL"
mkdir -p "$MNT"

# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"
read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== dd-bw on $(hostname -s) IB=$CLIENT_IB FILE_GIB=$FILE_GIB ==="

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in ${CPID:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    if [ -f "$LOCAL/client.perf.data" ]; then
        cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
        perf report --stdio --no-children --percent-limit 0.3 \
            -i "$PROF/client.perf.data" >"$PROF/client.report.txt" 2>/dev/null || true
        {
            echo "=== client hotspots ==="
            grep -E 'blake3|efs_|fuse_|memcpy|encode|decode|put_|get_|write|read|hash' \
                "$PROF/client.report.txt" || true
            echo "--- top ---"
            grep -E '^\s+[0-9]+\.[0-9]+%' "$PROF/client.report.txt" | head -25 || true
        } | tee "$PROF/client.hotpath.txt"
    fi
    echo "cleaning /scratch: $LOCAL"
    rm -rf "$LOCAL"
}
trap cleanup EXIT

for i in $(seq 1 "$NUM_SERVERS"); do
    efs_wait_addr "$i" 600
done
S1=$(cat "$SHARED/state/s1.addr")
echo "seed=$S1"
for i in $(seq 1 "$NUM_SERVERS"); do
    addr=$(cat "$SHARED/state/s${i}.addr")
    host=${addr%:*}
    port=${addr#*:}
    for _ in $(seq 1 120); do
        timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null && break
        sleep 1
    done
done

GO="$PROF_ROOT/GO"
for _ in $(seq 1 300); do
    [ -f "$GO" ] && break
    sleep 1
done
[ -f "$GO" ] || { echo "timeout GO"; exit 1; }
echo "GO $(date -Is)"

# --- Path ceiling: store-bench (no FUSE) ---
if [ "$DO_STORE_BENCH" = "1" ]; then
    echo "=== store-bench ${FILE_GIB}G ==="
    STORE_ARGS=()
    if [ "${EFS_BENCH_PERF:-0}" = "1" ]; then
        STORE_ARGS+=(--perf)
        export EFS_PERF_PATH="$LOCAL/store.perf.data"
    fi
    set +e
    "$REPO/efs-bench" "$S1" --size "${FILE_GIB}G" "${STORE_ARGS[@]}" \
        | tee "$PROF/store-bench.txt"
    set -e
    if [ -f "$LOCAL/store.perf.data" ]; then
        cp -f "$LOCAL/store.perf.data" "$PROF/store.perf.data" 2>/dev/null || true
        perf report --stdio --no-children --percent-limit 0.3 \
            -i "$PROF/store.perf.data" >"$PROF/store.report.txt" 2>/dev/null || true
        grep -E '^\s+[0-9]+\.[0-9]+%|blake3|efs_|memcpy' "$PROF/store.report.txt" \
            | head -40 | tee "$PROF/store.hotpath.txt" || true
    fi
fi

# --- FUSE single dd ---
FUSE_EXTRA=()
if [ "${EFS_FUSE_PERF:-0}" = "1" ]; then
    FUSE_EXTRA+=(--perf)
    export EFS_PERF_PATH="$LOCAL/client.perf.data"
fi
EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}" \
    "$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f "${FUSE_EXTRA[@]}" \
    >"$PROF/fuse.stdout" 2>&1 &
CPID=$!
for _ in $(seq 1 120); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { cat "$PROF/fuse.stdout"; exit 1; }
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/fuse.stdout"; exit 1; }

BYTES=$((FILE_GIB * 1024 * 1024 * 1024))
COUNT=$((FILE_GIB * 1024))

echo "=== dd WRITE ${FILE_GIB}GiB bs=1M if=/dev/zero ==="
W0=$(date +%s.%N)
dd if=/dev/zero of="$MNT/dd.bin" bs=1M count="$COUNT" status=progress conv=fsync 2>"$PROF/dd-write.err"
sync "$MNT/dd.bin" 2>/dev/null || true
W1=$(date +%s.%N)
W_SEC=$(awk -v s="$W0" -v e="$W1" 'BEGIN{printf "%.3f", e-s}')
W_GIB=$(awk -v b="$BYTES" -v t="$W_SEC" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
echo "WRITE wall_s=$W_SEC GiB_s=$W_GIB" | tee "$PROF/write.txt"
cat "$PROF/dd-write.err" | tee -a "$PROF/write.txt"

echo "=== dd READ ${FILE_GIB}GiB bs=1M of=/dev/null ==="
# Drop page cache on this node for the mount file if possible (best-effort)
R0=$(date +%s.%N)
dd if="$MNT/dd.bin" of=/dev/null bs=1M status=progress 2>"$PROF/dd-read.err"
R1=$(date +%s.%N)
R_SEC=$(awk -v s="$R0" -v e="$R1" 'BEGIN{printf "%.3f", e-s}')
R_GIB=$(awk -v b="$BYTES" -v t="$R_SEC" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
echo "READ wall_s=$R_SEC GiB_s=$R_GIB" | tee "$PROF/read.txt"
cat "$PROF/dd-read.err" | tee -a "$PROF/read.txt"

{
    echo "label=${LABEL:-dd-bw}"
    echo "write_GiB_s=$W_GIB write_wall_s=$W_SEC"
    echo "read_GiB_s=$R_GIB read_wall_s=$R_SEC"
    grep 'BENCH_OK' "$PROF/store-bench.txt" 2>/dev/null || true
} | tee "$PROF/SUMMARY.txt"

fusermount -u "$MNT" 2>/dev/null || true
kill -TERM "$CPID" 2>/dev/null || true
sleep 2
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

echo "DD_BW_DONE write_GiB_s=$W_GIB read_GiB_s=$R_GIB"
