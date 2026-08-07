#!/bin/bash
#SBATCH --job-name=efs-str-cli
#SBATCH --partition=mit_normal
#SBATCH --time=06:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-stream-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-stream-%j.err
#
# One IB client: mount export, write FILES_PER_CLIENT × FILE_GIB GiB files in
# parallel. CLIENT_ID distinguishes paths/logs across jobs.
# Env: ROUND, CLIENT_ID, FILE_GIB (default 100), FILES_PER_CLIENT (default 2),
#      EXPORT_NAME, PROF_ROOT, NUM_SERVERS (default 3; wait for s1..sN, seed fuse from s1)

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
CLIENT_ID="${CLIENT_ID:?CLIENT_ID required}"
FILE_GIB="${FILE_GIB:-100}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-2}"
EXPORT_NAME="${EXPORT_NAME:-streamexport}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_ROOT="${PROF_ROOT:-$SHARED/profile/stream-4x100-ib-r${ROUND}}"
PROF="$PROF_ROOT/client${CLIENT_ID}-${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs" "$PROF_ROOT"
if [ "$NUM_SERVERS" -lt 1 ] || [ "$NUM_SERVERS" -gt 4 ]; then
    echo "ERROR: NUM_SERVERS must be 1..4 (got $NUM_SERVERS)"
    exit 1
fi

DD_BS=$((1024 * 1024))
DD_COUNT=$((FILE_GIB * 1024))
FILE_BYTES=$((FILE_GIB * 1024 * 1024 * 1024))

if [ ! -d /scratch ] || ! mkdir -p "/scratch/efs-testing/${SLURM_JOB_ID}" 2>/dev/null; then
    echo "ERROR: client needs /scratch"
    exit 1
fi
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$LOCAL/mnt"
rm -rf "$LOCAL"
mkdir -p "$MNT"

# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== stream IB client_id=$CLIENT_ID on $(hostname -s) IB=$CLIENT_IB ($CLIENT_IP) ==="
echo "PROF=$PROF FILE_GIB=$FILE_GIB FILES_PER_CLIENT=$FILES_PER_CLIENT"

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
    if [ -n "${LOCAL:-}" ] && [ -f "$LOCAL/client.perf.data" ]; then
        cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
    fi
    # when the harness finishes, always cleanup after yourself in /scratch.
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

SERVER_ADDRS=()
for i in $(seq 1 "$NUM_SERVERS"); do
    WAITED=0
    while [ ! -f "$SHARED/state/s${i}.addr" ]; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 600 ]; then
            echo "Timed out waiting for s${i}.addr"
            exit 1
        fi
    done
    SERVER_ADDRS+=("$(cat "$SHARED/state/s${i}.addr")")
done
S1="${SERVER_ADDRS[0]}"
echo "servers (IB, n=$NUM_SERVERS): ${SERVER_ADDRS[*]}"

for addr in "${SERVER_ADDRS[@]}"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 600 ]; then
            echo "Timed out waiting for $addr over IB"
            exit 1
        fi
    done
    echo "reachable: $addr"
done

# Wait for orchestrator mkfs + GO barrier
GO="$PROF_ROOT/GO"
WAITED=0
while [ ! -f "$GO" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    if [ "$WAITED" -ge 600 ]; then
        echo "Timed out waiting for $GO"
        exit 1
    fi
done
echo "GO received"

# EFS_FUSE_PERF=1 enables perf record (adds overhead; off for load runs).
FUSE_EXTRA=()
if [ "${EFS_FUSE_PERF:-0}" = "1" ]; then
    FUSE_EXTRA+=(--perf)
    export EFS_PERF_PATH="$LOCAL/client.perf.data"
fi
# Seed with s1 only so LIST_NODES discovers full membership (3 or 4).
EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-16384}" \
    "$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f \
    "${FUSE_EXTRA[@]}" \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 120); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "client died:"; cat "$PROF/client.stdout"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/client.stdout"; exit 1; }
echo "mounted on $MNT via IB"

DIR="$MNT/c${CLIENT_ID}"
mkdir -p "$DIR"

stream_one() {
    local fid="$1"
    local path="$DIR/stream-${fid}.bin"
    local log="$PROF/writer-${fid}.log"
    {
        echo "[c${CLIENT_ID}-f${fid}] start $(date -Is) path=$path bytes=$FILE_BYTES"
        local start end elapsed got
        start=$(date +%s.%N)
        if dd if=/dev/zero of="$path" bs="$DD_BS" count="$DD_COUNT" status=progress; then
            end=$(date +%s.%N)
            elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
            got=$(stat -c%s "$path" 2>/dev/null || echo 0)
            local thr_mibs thr_gibs
            thr_mibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
            thr_gibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
            echo "[c${CLIENT_ID}-f${fid}] done $(date -Is) wall_sec=$elapsed bytes=$got MiB_s=$thr_mibs GiB_s=$thr_gibs"
            echo "$elapsed $got $thr_mibs $thr_gibs" > "$PROF/writer-${fid}.result"
        else
            echo "[c${CLIENT_ID}-f${fid}] FAILED rc=$?"
            echo "FAIL" > "$PROF/writer-${fid}.result"
            return 1
        fi
    } >"$log" 2>&1
}

echo "=== client $CLIENT_ID: ${FILES_PER_CLIENT} x ${FILE_GIB} GiB parallel ==="
echo "client_ib=$CLIENT_IB" > "$PROF/IB.txt"
echo "num_servers=$NUM_SERVERS" >> "$PROF/IB.txt"
echo "servers=${SERVER_ADDRS[*]}" >> "$PROF/IB.txt"
echo "node=$(hostname -s)" >> "$PROF/IB.txt"

WALL_START=$(date +%s.%N)
echo "$WALL_START" > "$PROF/wall_start.txt"
WPIDS=()
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    stream_one "$f" &
    WPIDS+=($!)
done
ok=1
for pid in "${WPIDS[@]}"; do
    wait "$pid" || ok=0
done
WALL_END=$(date +%s.%N)
echo "$WALL_END" > "$PROF/wall_end.txt"
WALL=$(awk -v s="$WALL_START" -v e="$WALL_END" 'BEGIN{printf "%.3f", e-s}')

total_bytes=0
echo "=== client $CLIENT_ID results ==="
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    echo "--- file $f ---"
    tail -3 "$PROF/writer-${f}.log" 2>/dev/null || true
    if [ -f "$PROF/writer-${f}.result" ] && ! grep -q FAIL "$PROF/writer-${f}.result"; then
        read -r el got mibs gibs < "$PROF/writer-${f}.result"
        total_bytes=$((total_bytes + got))
        echo "file_${f}_wall_sec=$el file_${f}_bytes=$got file_${f}_MiB_s=$mibs"
    else
        ok=0
        echo "file_${f}=FAILED"
    fi
done
cli_mibs=$(awk -v b="$total_bytes" -v t="$WALL" 'BEGIN{printf "%.2f", (b/1024/1024)/(t<0.001?0.001:t)}')
cli_gibs=$(awk -v b="$total_bytes" -v t="$WALL" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
echo "client_${CLIENT_ID}_write_wall_sec=$WALL client_${CLIENT_ID}_write_bytes=$total_bytes client_${CLIENT_ID}_write_MiB_s=$cli_mibs client_${CLIENT_ID}_write_GiB_s=$cli_gibs ok=$ok"
echo "$WALL $total_bytes $cli_mibs $cli_gibs $ok" > "$PROF/SUMMARY.write.txt"
echo "$WALL $total_bytes $cli_mibs $cli_gibs $ok" > "$PROF/SUMMARY.txt"

# Optional read-back phase (parallel dd of the files just written).
READ_OK=1
READ_WALL=0
READ_BYTES=0
if [ "${DO_READ:-1}" = "1" ]; then
    echo "=== client $CLIENT_ID: parallel read-back ==="
    read_one() {
        local fid="$1"
        local path="$DIR/stream-${fid}.bin"
        local log="$PROF/reader-${fid}.log"
        {
            local start end elapsed got
            start=$(date +%s.%N)
            if dd if="$path" of=/dev/null bs="$DD_BS" status=progress; then
                end=$(date +%s.%N)
                elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
                got=$(stat -c%s "$path" 2>/dev/null || echo 0)
                local thr_mibs thr_gibs
                thr_mibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
                thr_gibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
                echo "[c${CLIENT_ID}-f${fid}] read done wall_sec=$elapsed bytes=$got MiB_s=$thr_mibs GiB_s=$thr_gibs"
                echo "$elapsed $got $thr_mibs $thr_gibs" > "$PROF/reader-${fid}.result"
            else
                echo "[c${CLIENT_ID}-f${fid}] READ FAILED"
                echo "FAIL" > "$PROF/reader-${fid}.result"
                return 1
            fi
        } >"$log" 2>&1
    }
    RSTART=$(date +%s.%N)
    RPIDS=()
    for f in $(seq 1 "$FILES_PER_CLIENT"); do
        read_one "$f" &
        RPIDS+=($!)
    done
    for pid in "${RPIDS[@]}"; do
        wait "$pid" || READ_OK=0
    done
    REND=$(date +%s.%N)
    READ_WALL=$(awk -v s="$RSTART" -v e="$REND" 'BEGIN{printf "%.3f", e-s}')
    for f in $(seq 1 "$FILES_PER_CLIENT"); do
        if [ -f "$PROF/reader-${f}.result" ] && ! grep -q FAIL "$PROF/reader-${f}.result"; then
            read -r el got mibs gibs < "$PROF/reader-${f}.result"
            READ_BYTES=$((READ_BYTES + got))
            echo "read_file_${f}_wall_sec=$el read_file_${f}_MiB_s=$mibs"
        else
            READ_OK=0
        fi
    done
    read_mibs=$(awk -v b="$READ_BYTES" -v t="$READ_WALL" 'BEGIN{printf "%.2f", (b/1024/1024)/(t<0.001?0.001:t)}')
    read_gibs=$(awk -v b="$READ_BYTES" -v t="$READ_WALL" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
    echo "client_${CLIENT_ID}_read_wall_sec=$READ_WALL client_${CLIENT_ID}_read_bytes=$READ_BYTES client_${CLIENT_ID}_read_MiB_s=$read_mibs client_${CLIENT_ID}_read_GiB_s=$read_gibs ok=$READ_OK"
    echo "$READ_WALL $READ_BYTES $read_mibs $read_gibs $READ_OK" > "$PROF/SUMMARY.read.txt"
    [ "$READ_OK" = "1" ] || ok=0
fi

# Unmount before perf copy in cleanup
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
for i in $(seq 1 60); do
    kill -0 "$CPID" 2>/dev/null || break
    sleep 0.5
done
kill -TERM "$CPID" 2>/dev/null || true
sleep 3
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

if [ -f "$LOCAL/client.perf.data" ]; then
    cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/client.perf.data" \
        > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || true
fi

echo "Stream client $CLIENT_ID done: $PROF"
[ "$ok" = "1" ]
