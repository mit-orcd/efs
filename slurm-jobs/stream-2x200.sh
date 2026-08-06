#!/bin/bash
#SBATCH --job-name=efs-stream2
# Needs node-local /scratch with ~2x logical bytes free (EC + headroom).
#SBATCH --partition=mit_normal
#SBATCH --time=06:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/stream-2x200-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/stream-2x200-%j.err
#
# Two FUSE clients each stream-write one large file in parallel; report
# per-client and aggregate throughput.
# Override size: FILE_GIB=8 sbatch slurm-jobs/stream-2x200.sh

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PROF="$SHARED/profile/stream-2x200-${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs"

FILE_GIB="${FILE_GIB:-200}"
# 1 MiB blocks; count = GiB * 1024
DD_BS=$((1024 * 1024))
DD_COUNT=$((FILE_GIB * 1024))
FILE_BYTES=$((FILE_GIB * 1024 * 1024 * 1024))

# Require real /scratch — root /tmp is too small for multi-hundred-GiB loads.
if [ ! -d /scratch ] || ! mkdir -p "/scratch/efs-testing/${SLURM_JOB_ID}" 2>/dev/null; then
    echo "ERROR: this job requires node-local /scratch (mit_normal). Got hostname=$(hostname)"
    exit 1
fi
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$LOCAL"
echo "LOCAL=$LOCAL FILE_GIB=$FILE_GIB FILE_BYTES=$FILE_BYTES"

IP="127.0.0.1"
P1=1991
P2=1992
P3=1993
MNT1="$LOCAL/mnt1"
MNT2="$LOCAL/mnt2"

cleanup() {
    set +e
    for m in "$MNT1" "$MNT2"; do
        fusermount -u "$m" 2>/dev/null || umount "$m" 2>/dev/null || true
    done
    for pid in ${C1:-} ${C2:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in ${C1:-} ${C2:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    # when the harness finishes, always cleanup after yourself in /scratch.
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

echo "=== stream 2x${FILE_GIB}GiB on $(hostname) ==="
df -h "$LOCAL" | tail -1
avail_kb=$(df -Pk "$LOCAL" | awk 'NR==2{print $4}')
# Budget ~2x logical for 1.5x EC + metadata/scratch headroom.
need_kb=$((FILE_GIB * 2 * 2 * 1024 * 1024))
if [ "$avail_kb" -lt "$need_kb" ]; then
    echo "ERROR: $LOCAL has ${avail_kb}KiB free, need >= ${need_kb}KiB for 2x${FILE_GIB}GiB"
    exit 1
fi

fusermount -u "$MNT1" 2>/dev/null || true
fusermount -u "$MNT2" 2>/dev/null || true
rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT1" "$MNT2" "$PROF"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    > "$PROF/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 60); do
    grep -q "listening on" "$PROF/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { echo "s1 died:"; cat "$PROF/s1.stdout"; exit 1; }
    sleep 0.5
done
grep -q "listening on" "$PROF/s1.stdout" || { echo "s1 not ready"; exit 1; }
echo "server 1 ready"

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    > "$PROF/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    > "$PROF/s3.stdout" 2>&1 &
S3=$!
for port in "$P2" "$P3"; do
    READY=0
    for i in $(seq 1 60); do
        timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null && READY=1 && break
        sleep 0.5
    done
    [ "$READY" = "1" ] || { echo "port $port down"; cat "$PROF"/s*.stdout; exit 1; }
done
sleep 2
echo "cluster up"

MKFS_OUT=$("$REPO/efs-mgmt" mkfs "$IP:$P1" streamexport 2>&1) || true
echo "mkfs: $MKFS_OUT"
echo "$MKFS_OUT" | grep -q "created\|already exists" || { echo "mkfs failed"; exit 1; }

EFS_META_BATCH_OPS=16384 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" streamexport "$MNT1" -f \
    > "$PROF/client1.stdout" 2>&1 &
C1=$!
EFS_META_BATCH_OPS=16384 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" streamexport "$MNT2" -f \
    > "$PROF/client2.stdout" 2>&1 &
C2=$!
for m in "$MNT1" "$MNT2"; do
    for i in $(seq 1 60); do
        mountpoint -q "$m" 2>/dev/null && break
        sleep 0.5
    done
    mountpoint -q "$m" || { echo "mount failed: $m"; cat "$PROF"/client*.stdout; exit 1; }
done
echo "both clients mounted"

# Distinct paths so the two clients do not contend on the same inode name.
stream_one() {
    local id="$1" mnt="$2"
    local path="$mnt/stream-${id}.bin"
    local log="$PROF/writer-${id}.log"
    {
        echo "[client-$id] start $(date -Is) path=$path bytes=$FILE_BYTES bs=$DD_BS count=$DD_COUNT"
        local start end elapsed
        start=$(date +%s)
        # Large sequential write; status=progress → stderr for the log.
        if dd if=/dev/zero of="$path" bs="$DD_BS" count="$DD_COUNT" status=progress; then
            end=$(date +%s)
            elapsed=$((end - start))
            [ "$elapsed" -lt 1 ] && elapsed=1
            local got
            got=$(stat -c%s "$path" 2>/dev/null || echo 0)
            local mibs thr_mibs
            mibs=$(awk -v b="$got" 'BEGIN{printf "%.2f", b/1024/1024}')
            thr_mibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
            thr_gibs=$(awk -v b="$got" -v t="$elapsed" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
            echo "[client-$id] done $(date -Is) wall_sec=$elapsed bytes=$got MiB=$mibs MiB_s=$thr_mibs GiB_s=$thr_gibs"
            echo "$elapsed $got" > "$PROF/writer-${id}.result"
        else
            echo "[client-$id] FAILED rc=$?"
            echo "FAIL" > "$PROF/writer-${id}.result"
            return 1
        fi
    } >"$log" 2>&1
}

echo "=== streaming 2 clients x ${FILE_GIB} GiB ==="
WALL_START=$(date +%s)
stream_one 1 "$MNT1" &
W1=$!
stream_one 2 "$MNT2" &
W2=$!
wait "$W1" || true
wait "$W2" || true
WALL_END=$(date +%s)
WALL=$((WALL_END - WALL_START))
[ "$WALL" -lt 1 ] && WALL=1

echo "=== results ==="
ok=1
total_bytes=0
for id in 1 2; do
    echo "--- client $id ---"
    cat "$PROF/writer-${id}.log" | tail -5
    if [ -f "$PROF/writer-${id}.result" ] && ! grep -q FAIL "$PROF/writer-${id}.result"; then
        read -r el got < "$PROF/writer-${id}.result"
        total_bytes=$((total_bytes + got))
        echo "client_${id}_wall_sec=$el client_${id}_bytes=$got"
    else
        ok=0
        echo "client_${id}=FAILED"
    fi
done

agg_mibs=$(awk -v b="$total_bytes" -v t="$WALL" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
agg_gibs=$(awk -v b="$total_bytes" -v t="$WALL" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
echo "aggregate_wall_sec=$WALL aggregate_bytes=$total_bytes aggregate_MiB_s=$agg_mibs aggregate_GiB_s=$agg_gibs"
echo "ok=$ok"
echo "$WALL $total_bytes $agg_mibs $agg_gibs $ok" > "$PROF/SUMMARY.txt"
ln -sfn "$PROF" "$SHARED/profile/stream-latest"
echo "Stream test done: $PROF"
[ "$ok" = "1" ]
