#!/bin/bash
#SBATCH --job-name=efs-dd-mix
#SBATCH --partition=mit_normal
#SBATCH --time=01:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=32G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/dd-mix-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/dd-mix-%j.err
#
# Three sequential FUSE workloads with per-phase client perf:
#   1) 16× parallel dd: 100 files × 100 KiB each
#   2) 16× parallel dd: 1000 files × 10 KiB each
#   3) 4× parallel dd: 1 file × 10 GiB each
#
# Env: EXPORT_NAME, PROF_ROOT, NUM_SERVERS, LABEL

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
EXPORT_NAME="${EXPORT_NAME:-ddmix}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_ROOT="${PROF_ROOT:?PROF_ROOT required}"
PROF="$PROF_ROOT/client-${SLURM_JOB_ID}"
LABEL="${LABEL:-dd-mix}"
mkdir -p "$PROF" "$SHARED/logs"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"
# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"

LOCAL=$(efs_job_scratch) || exit 1
MNT=$LOCAL/mnt
rm -rf "$LOCAL"
mkdir -p "$MNT"

read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== dd-mix on $(hostname -s) IB=$CLIENT_IB LABEL=$LABEL ==="

CPID=""
PERF_PID=""
cleanup() {
    set +e
    if [ -n "${PERF_PID:-}" ]; then
        kill -INT "$PERF_PID" 2>/dev/null || true
        wait "$PERF_PID" 2>/dev/null || true
        PERF_PID=""
    fi
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    if [ -n "${CPID:-}" ]; then
        kill -INT "$CPID" 2>/dev/null || true
        sleep 2
        kill -KILL "$CPID" 2>/dev/null || true
        wait "$CPID" 2>/dev/null || true
        CPID=""
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

EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}" \
    "$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f \
    >"$PROF/fuse.stdout" 2>&1 &
CPID=$!
for _ in $(seq 1 120); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { cat "$PROF/fuse.stdout"; exit 1; }
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/fuse.stdout"; exit 1; }

# --- helpers ---------------------------------------------------------------
start_phase_perf() {
    local phase=$1
    PERF_PID=""
    if ! command -v perf >/dev/null 2>&1; then
        echo "perf not available; skipping hotpath for $phase"
        return 0
    fi
    local pdata="$LOCAL/${phase}.perf.data"
    rm -f "$pdata"
    perf record -g -F 999 -p "$CPID" -o "$pdata" -- sleep 86400 >/dev/null 2>&1 &
    PERF_PID=$!
    sleep 1
    if ! kill -0 "$PERF_PID" 2>/dev/null; then
        echo "perf record failed to start for $phase"
        PERF_PID=""
    fi
}

stop_phase_perf() {
    local phase=$1
    local pdata="$LOCAL/${phase}.perf.data"
    local outdir="$PROF/$phase"
    mkdir -p "$outdir"
    if [ -z "${PERF_PID:-}" ]; then
        echo "(no perf for $phase)" | tee "$outdir/hotpath.txt"
        return 0
    fi
    kill -INT "$PERF_PID" 2>/dev/null || true
    for _ in $(seq 1 40); do
        kill -0 "$PERF_PID" 2>/dev/null || break
        sleep 0.25
    done
    kill -KILL "$PERF_PID" 2>/dev/null || true
    wait "$PERF_PID" 2>/dev/null || true
    PERF_PID=""
    if [ -f "$pdata" ]; then
        cp -f "$pdata" "$outdir/client.perf.data" 2>/dev/null || true
        perf report --stdio --no-children --percent-limit 0.3 \
            -i "$outdir/client.perf.data" >"$outdir/client.report.txt" 2>/dev/null || true
        {
            echo "=== $phase client hotspots ==="
            grep -E 'blake3|efs_|fuse_|memcpy|encode|decode|put_|get_|write|read|hash|memset|memcmp' \
                "$outdir/client.report.txt" || true
            echo "--- top ---"
            grep -E '^\s+[0-9]+\.[0-9]+%' "$outdir/client.report.txt" | head -25 || true
        } | tee "$outdir/hotpath.txt"
    else
        echo "(perf data missing for $phase)" | tee "$outdir/hotpath.txt"
    fi
}

# Run N workers in parallel; each worker runs the given bash snippet with
# WORKER_ID set. Returns aggregate wall seconds via REPLY_WALL.
run_parallel_workers() {
    local nworkers=$1
    shift
    local worker_script=$1
    local pids=()
    local w
    local t0 t1
    t0=$(date +%s.%N)
    for w in $(seq 0 $((nworkers - 1))); do
        (
            export WORKER_ID=$w
            export MNT
            bash -c "$worker_script"
        ) &
        pids+=($!)
    done
    local fail=0
    for pid in "${pids[@]}"; do
        if ! wait "$pid"; then
            fail=1
        fi
    done
    t1=$(date +%s.%N)
    REPLY_WALL=$(awk -v s="$t0" -v e="$t1" 'BEGIN{printf "%.3f", e-s}')
    return $fail
}

summarize_phase() {
    local phase=$1
    local wall=$2
    local bytes=$3
    local files=$4
    local outdir="$PROF/$phase"
    mkdir -p "$outdir"
    local gibs files_s
    gibs=$(awk -v b="$bytes" -v t="$wall" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
    files_s=$(awk -v f="$files" -v t="$wall" 'BEGIN{printf "%.1f", f/(t<0.001?0.001:t)}')
    {
        echo "phase=$phase"
        echo "wall_s=$wall"
        echo "bytes=$bytes"
        echo "files=$files"
        echo "GiB_s=$gibs"
        echo "files_s=$files_s"
    } | tee "$outdir/SUMMARY.txt"
}

# --- Phase 1: 16 workers × 100 × 100 KiB ----------------------------------
PHASE=p1_16x100x100k
echo "=== $PHASE ==="
mkdir -p "$MNT/$PHASE"
start_phase_perf "$PHASE"
P1_SCRIPT='
set -euo pipefail
d="$MNT/'"$PHASE"'/w$(printf "%02d" "$WORKER_ID")"
mkdir -p "$d"
for i in $(seq -w 0 99); do
    dd if=/dev/zero of="$d/f$i" bs=102400 count=1 status=none
done
sync "$d" 2>/dev/null || true
'
if run_parallel_workers 16 "$P1_SCRIPT"; then
    echo "phase1 workers OK wall_s=$REPLY_WALL"
else
    echo "phase1 workers FAILED wall_s=$REPLY_WALL"
fi
# Drop page cache best-effort before sync accounting
sync "$MNT/$PHASE" 2>/dev/null || true
stop_phase_perf "$PHASE"
# 16*100*100KiB
summarize_phase "$PHASE" "$REPLY_WALL" $((16 * 100 * 100 * 1024)) $((16 * 100))

# --- Phase 2: 16 workers × 1000 × 10 KiB ----------------------------------
PHASE=p2_16x1000x10k
echo "=== $PHASE ==="
mkdir -p "$MNT/$PHASE"
start_phase_perf "$PHASE"
P2_SCRIPT='
set -euo pipefail
d="$MNT/'"$PHASE"'/w$(printf "%02d" "$WORKER_ID")"
mkdir -p "$d"
for i in $(seq -w 0 999); do
    dd if=/dev/zero of="$d/f$i" bs=10240 count=1 status=none
done
sync "$d" 2>/dev/null || true
'
if run_parallel_workers 16 "$P2_SCRIPT"; then
    echo "phase2 workers OK wall_s=$REPLY_WALL"
else
    echo "phase2 workers FAILED wall_s=$REPLY_WALL"
fi
sync "$MNT/$PHASE" 2>/dev/null || true
stop_phase_perf "$PHASE"
# 16*1000*10KiB
summarize_phase "$PHASE" "$REPLY_WALL" $((16 * 1000 * 10 * 1024)) $((16 * 1000))

# --- Phase 3: 4 workers × 1 × 10 GiB --------------------------------------
PHASE=p3_4x1x10g
echo "=== $PHASE ==="
mkdir -p "$MNT/$PHASE"
start_phase_perf "$PHASE"
P3_SCRIPT='
set -euo pipefail
# 10 GiB = 10240 MiB
dd if=/dev/zero of="$MNT/'"$PHASE"'/w${WORKER_ID}.bin" bs=1M count=10240 status=progress conv=fsync
'
if run_parallel_workers 4 "$P3_SCRIPT"; then
    echo "phase3 workers OK wall_s=$REPLY_WALL"
else
    echo "phase3 workers FAILED wall_s=$REPLY_WALL"
fi
sync "$MNT/$PHASE" 2>/dev/null || true
stop_phase_perf "$PHASE"
# 4*10GiB
summarize_phase "$PHASE" "$REPLY_WALL" $((4 * 10 * 1024 * 1024 * 1024)) 4

# --- Final rollup ---------------------------------------------------------
{
    echo "label=$LABEL"
    echo "client=$(hostname -s) ib=$CLIENT_IB"
    for p in p1_16x100x100k p2_16x1000x10k p3_4x1x10g; do
        echo "----- $p -----"
        cat "$PROF/$p/SUMMARY.txt" 2>/dev/null || echo "(missing)"
    done
} | tee "$PROF/SUMMARY.txt"

echo "DD_MIX_DONE"
fusermount -u "$MNT" 2>/dev/null || true
kill -TERM "$CPID" 2>/dev/null || true
sleep 2
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""
