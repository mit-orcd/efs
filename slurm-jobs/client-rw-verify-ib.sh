#!/bin/bash
#SBATCH --job-name=efs-rw-cli
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-rw-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-rw-%j.err
#
# One IB client: write FILES_PER_CLIENT × FILE_GIB GiB from local urandom
# sources, sha256-verify after write, timed readback + sha256 (corruption
# check), optional --perf.
#
# Env: ROUND, CLIENT_ID, FILE_GIB, FILES_PER_CLIENT, EXPORT_NAME, PROF_ROOT,
#      NUM_SERVERS, EFS_FUSE_PERF=0|1

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
CLIENT_ID="${CLIENT_ID:?CLIENT_ID required}"
FILE_GIB="${FILE_GIB:-1}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-1}"
EXPORT_NAME="${EXPORT_NAME:-rwverify}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_ROOT="${PROF_ROOT:-$SHARED/profile/rw-verify-r${ROUND}}"
PROF="$PROF_ROOT/client${CLIENT_ID}-${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs" "$PROF_ROOT"

DD_BS=$((1024 * 1024))
DD_COUNT=$((FILE_GIB * 1024))
FILE_BYTES=$((FILE_GIB * 1024 * 1024 * 1024))

if [ ! -d /scratch ] || ! mkdir -p "/scratch/efs-testing/${SLURM_JOB_ID}" 2>/dev/null; then
    echo "ERROR: client needs /scratch"
    exit 1
fi
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$LOCAL/mnt"
SRC="$LOCAL/src"
rm -rf "$LOCAL"
mkdir -p "$MNT" "$SRC"

# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== rw-verify client_id=$CLIENT_ID on $(hostname -s) IB=$CLIENT_IB ($CLIENT_IP) ==="
echo "PROF=$PROF FILE_GIB=$FILE_GIB FILES=$FILES_PER_CLIENT"

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
        [ "$WAITED" -ge 600 ] && { echo "timeout s${i}.addr"; exit 1; }
    done
    SERVER_ADDRS+=("$(cat "$SHARED/state/s${i}.addr")")
done
S1="${SERVER_ADDRS[0]}"
echo "servers: ${SERVER_ADDRS[*]}"

for addr in "${SERVER_ADDRS[@]}"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        [ "$WAITED" -ge 600 ] && { echo "timeout $addr"; exit 1; }
    done
done

GO="$PROF_ROOT/GO"
WAITED=0
while [ ! -f "$GO" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    [ "$WAITED" -ge 600 ] && { echo "timeout GO"; exit 1; }
done
echo "GO received"

FUSE_EXTRA=()
if [ "${EFS_FUSE_PERF:-0}" = "1" ]; then
    FUSE_EXTRA+=(--perf)
    export EFS_PERF_PATH="$LOCAL/client.perf.data"
fi

EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}" \
    "$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f \
    "${FUSE_EXTRA[@]}" \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 120); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "fuse died"; cat "$PROF/client.stdout"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/client.stdout"; exit 1; }

DIR="$MNT/c${CLIENT_ID}"
mkdir -p "$DIR"

# --- generate local sources + sha256 (integrity ground truth) ---
echo "=== generate ${FILES_PER_CLIENT} × ${FILE_GIB} GiB urandom sources ==="
GEN_START=$(date +%s.%N)
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    dd if=/dev/urandom of="$SRC/src-${f}.bin" bs="$DD_BS" count="$DD_COUNT" status=none
    sha256sum "$SRC/src-${f}.bin" | tee "$SRC/src-${f}.sha" | tee "$PROF/src-${f}.sha"
done
GEN_END=$(date +%s.%N)
GEN_SEC=$(awk -v s="$GEN_START" -v e="$GEN_END" 'BEGIN{printf "%.3f", e-s}')
echo "generate_wall_s=$GEN_SEC"

# --- parallel writes ---
write_one() {
    local fid=$1
    local src="$SRC/src-${fid}.bin"
    local dst="$DIR/rw-${fid}.bin"
    local log="$PROF/write-${fid}.log"
    {
        local start end elapsed got thr_mibs thr_gibs
        start=$(date +%s.%N)
        dd if="$src" of="$dst" bs="$DD_BS" status=none
        # Push through FUSE meta/data path
        sync "$dst" 2>/dev/null || true
        end=$(date +%s.%N)
        elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
        got=$(stat -c%s "$dst")
        thr_mibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
        thr_gibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
        echo "write wall_s=$elapsed bytes=$got MiB_s=$thr_mibs GiB_s=$thr_gibs"
        echo "$elapsed $got $thr_mibs $thr_gibs" > "$PROF/write-${fid}.result"
        # Integrity immediately after write (hash mount file vs source)
        local h_src h_dst
        h_src=$(awk '{print $1}' "$SRC/src-${fid}.sha")
        h_dst=$(sha256sum "$dst" | awk '{print $1}')
        if [ "$h_src" = "$h_dst" ] && [ "$got" = "$FILE_BYTES" ]; then
            echo "INTEGRITY_AFTER_WRITE OK $h_dst"
            echo "OK $h_dst" > "$PROF/integrity-write-${fid}.txt"
        else
            echo "INTEGRITY_AFTER_WRITE FAIL src=$h_src dst=$h_dst bytes=$got"
            echo "FAIL src=$h_src dst=$h_dst" > "$PROF/integrity-write-${fid}.txt"
            return 1
        fi
    } >"$log" 2>&1
}

echo "=== WRITE phase ==="
W_START=$(date +%s.%N)
WPIDS=()
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    write_one "$f" &
    WPIDS+=($!)
done
ok=1
for pid in "${WPIDS[@]}"; do
    wait "$pid" || ok=0
done
W_END=$(date +%s.%N)
W_WALL=$(awk -v s="$W_START" -v e="$W_END" 'BEGIN{printf "%.3f", e-s}')

# --- parallel readback + verify (corruption check) ---
read_one() {
    local fid=$1
    local dst="$DIR/rw-${fid}.bin"
    local out="$SRC/readback-${fid}.bin"
    local log="$PROF/read-${fid}.log"
    {
        local start end elapsed got thr_mibs thr_gibs h_src h_rb
        start=$(date +%s.%N)
        # Full read into scratch then hash (forces complete transfer + verify)
        dd if="$dst" of="$out" bs="$DD_BS" status=none
        end=$(date +%s.%N)
        elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN{printf "%.3f", e-s}')
        got=$(stat -c%s "$out")
        thr_mibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
        thr_gibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
        echo "read wall_s=$elapsed bytes=$got MiB_s=$thr_mibs GiB_s=$thr_gibs"
        echo "$elapsed $got $thr_mibs $thr_gibs" > "$PROF/read-${fid}.result"
        h_src=$(awk '{print $1}' "$SRC/src-${fid}.sha")
        h_rb=$(sha256sum "$out" | awk '{print $1}')
        if [ "$h_src" = "$h_rb" ] && [ "$got" = "$FILE_BYTES" ]; then
            echo "INTEGRITY_READBACK OK $h_rb"
            echo "OK $h_rb" > "$PROF/integrity-read-${fid}.txt"
        else
            echo "INTEGRITY_READBACK FAIL src=$h_src read=$h_rb bytes=$got expect=$FILE_BYTES"
            echo "FAIL src=$h_src read=$h_rb" > "$PROF/integrity-read-${fid}.txt"
            return 1
        fi
        rm -f "$out"
    } >"$log" 2>&1
}

echo "=== READBACK + VERIFY phase ==="
R_START=$(date +%s.%N)
RPIDS=()
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    read_one "$f" &
    RPIDS+=($!)
done
for pid in "${RPIDS[@]}"; do
    wait "$pid" || ok=0
done
R_END=$(date +%s.%N)
R_WALL=$(awk -v s="$R_START" -v e="$R_END" 'BEGIN{printf "%.3f", e-s}')

# Aggregate
w_bytes=0
r_bytes=0
for f in $(seq 1 "$FILES_PER_CLIENT"); do
    echo "--- file $f write ---"; tail -5 "$PROF/write-${f}.log" || true
    echo "--- file $f read ---"; tail -5 "$PROF/read-${f}.log" || true
    if [ -f "$PROF/write-${f}.result" ]; then
        read -r _ b _ _ < "$PROF/write-${f}.result"
        w_bytes=$((w_bytes + b))
    else
        ok=0
    fi
    if [ -f "$PROF/read-${f}.result" ]; then
        read -r _ b _ _ < "$PROF/read-${f}.result"
        r_bytes=$((r_bytes + b))
    else
        ok=0
    fi
    grep -q '^OK ' "$PROF/integrity-write-${f}.txt" 2>/dev/null || ok=0
    grep -q '^OK ' "$PROF/integrity-read-${f}.txt" 2>/dev/null || ok=0
done

w_gibs=$(awk -v b="$w_bytes" -v t="$W_WALL" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')
r_gibs=$(awk -v b="$r_bytes" -v t="$R_WALL" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/(t<0.001?0.001:t)}')

{
    echo "client_id=$CLIENT_ID ok=$ok"
    echo "write_wall_s=$W_WALL write_bytes=$w_bytes write_GiB_s=$w_gibs"
    echo "read_wall_s=$R_WALL read_bytes=$r_bytes read_GiB_s=$r_gibs"
    echo "integrity=$( [ "$ok" = 1 ] && echo PASS || echo FAIL )"
} | tee "$PROF/SUMMARY.txt"

echo "$W_WALL $w_bytes $w_gibs $R_WALL $r_bytes $r_gibs $ok" > "$PROF/metrics.txt"

fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
for i in $(seq 1 60); do
    kill -0 "$CPID" 2>/dev/null || break
    sleep 0.5
done
kill -TERM "$CPID" 2>/dev/null || true
sleep 2
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

if [ -f "$LOCAL/client.perf.data" ]; then
    cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
    perf report --stdio --no-children --percent-limit 0.4 -i "$PROF/client.perf.data" \
        > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || true
    {
        echo "=== efs / blake3 / network hotspots ==="
        grep -E 'blake3|efs_|fuse_|put_frag|get_frag|encode|decode|memcpy|sha256|hash' \
            "$PROF/client.report.txt" || true
    } | tee "$PROF/client.hotpath.txt"
fi

echo "client $CLIENT_ID done ok=$ok"
[ "$ok" = "1" ]
