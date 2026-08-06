#!/bin/bash
#SBATCH --job-name=efs-prof-heavy
# Prefer mit_normal; fall back to mit_preemptable when normal is full.
#SBATCH --partition=mit_normal,mit_preemptable
#SBATCH --time=08:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-heavy-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-heavy-%j.err
#
# Heavy profiling load: 100 GiB across 1,000,000 files (sharded), with --perf
# on server 1 and the FUSE client. Round id is optional: ROUND=1 sbatch ...

set -euo pipefail
# Re-exec once under line-buffered stdio so progress shows during long phases.
if [ -z "${EFS_HEAVY_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_HEAVY_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
PROF="$SHARED/profile/heavy-r${ROUND}-${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs"

# Node-local storage: /scratch when present (mit_normal), else $TMPDIR, else
# the shared parallel FS (slower, but always writable).
if [ ! -d /scratch ]; then
    echo "ERROR: /scratch required on compute node"
    exit 1
fi
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
rm -rf "$LOCAL"
mkdir -p "$LOCAL"
echo "LOCAL=$LOCAL"

IP="127.0.0.1"
P1=1991
P2=1992
P3=1993
MNT="$LOCAL/mnt"

# 1e6 files * 107374 bytes ~= 100 GiB logical.
NFILES=1000000
NDIRS=1000
FILE_BYTES=107374
WRITERS=4

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    for f in server.perf.data client.perf.data; do
        [ -f "$LOCAL/$f" ] && cp -f "$LOCAL/$f" "$PROF/$f" 2>/dev/null || true
    done
    # when the harness finishes, always cleanup after yourself in /scratch.
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

echo "=== heavy profile round $ROUND on $(hostname) ==="
echo "PROF=$PROF LOCAL=$LOCAL"
df -h "$LOCAL" | tail -1
avail_kb=$(df -Pk "$LOCAL" | awk 'NR==2{print $4}')
need_kb=$((220 * 1024 * 1024)) # ~220 GiB headroom for 1.5x EC + meta
if [ "$avail_kb" -lt "$need_kb" ]; then
    echo "ERROR: $LOCAL has ${avail_kb}KiB free, need >= ${need_kb}KiB"
    exit 1
fi

fusermount -u "$MNT" 2>/dev/null || true
rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$PROF"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

EFS_PERF_PATH="$LOCAL/server.perf.data" \
    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --perf \
    > "$PROF/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 60); do
    grep -q "listening on" "$PROF/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { echo "s1 died:"; cat "$PROF/s1.stdout"; exit 1; }
    sleep 0.5
done
grep -q "listening on" "$PROF/s1.stdout" || { echo "s1 not ready"; cat "$PROF/s1.stdout"; exit 1; }
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

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" profexport 2>&1)"

# Larger batch for the 1M-file load: ~60 full metadata flushes instead of ~244.
EFS_PERF_PATH="$LOCAL/client.perf.data" \
EFS_META_BATCH_OPS=16384 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" profexport "$MNT" -f --perf \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 60); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "client died:"; cat "$PROF/client.stdout"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/client.stdout"; exit 1; }
echo "mounted"

# Sharded tree: d000..d999 / f000000.. each FILE_BYTES of zero data.
# WRITERS parallel processes each own a contiguous dir range.
echo "=== load: ${NFILES} files x ${FILE_BYTES} bytes (~100 GiB), ${WRITERS} writers ==="
START=$(date +%s)
mkdir -p "$MNT/tree"
per_writer=$((NDIRS / WRITERS))
WPIDS=()
for w in $(seq 0 $((WRITERS - 1))); do
    (
        d0=$((w * per_writer))
        d1=$((d0 + per_writer))
        [ "$w" -eq $((WRITERS - 1)) ] && d1=$NDIRS
        files_per_dir=$((NFILES / NDIRS))
        buf="$LOCAL/buf.$w"
        # One reusable buffer per writer (zeros).
        dd if=/dev/zero of="$buf" bs="$FILE_BYTES" count=1 status=none
        n=0
        for ((d=d0; d<d1; d++)); do
            dir=$(printf "%s/tree/d%03d" "$MNT" "$d")
            mkdir -p "$dir"
            for ((f=0; f<files_per_dir; f++)); do
                path=$(printf "%s/f%06d" "$dir" "$f")
                cp "$buf" "$path"
                n=$((n + 1))
                if (( n % 5000 == 0 )); then
                    echo "[writer-$w] $n files ($(date +%H:%M:%S))"
                fi
            done
        done
        echo "[writer-$w] done $n files"
    ) > "$PROF/writer-$w.log" 2>&1 &
    WPIDS+=($!)
done
# Wait only for writers — not fuse/server background jobs.
for pid in "${WPIDS[@]}"; do
    wait "$pid" || true
done
END=$(date +%s)
echo "write_wall_sec=$((END - START))"

echo "=== sample listing ==="
ls "$MNT/tree" | wc -l
ls "$MNT/tree/d000" | wc -l
# Skip du -sh over 1M FUSE files — it can dominate wall time after the write.

echo "=== load: sequential read of 1000 sample files ==="
START=$(date +%s)
for d in 0 100 200 300 400 500 600 700 800 900; do
    dir=$(printf "%s/tree/d%03d" "$MNT" "$d")
    for f in 0 100 200 300 400 500 600 700 800 900; do
        path=$(printf "%s/f%06d" "$dir" "$f")
        dd if="$path" of=/dev/null bs=128K status=none
    done
done
END=$(date +%s)
echo "sample_read_wall_sec=$((END - START))"
echo "=== load done ==="

# Teardown for perf flush.
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

kill -INT "$S1" "$S2" "$S3" 2>/dev/null || true
for i in $(seq 1 40); do
    alive=0
    for pid in "$S1" "$S2" "$S3"; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ "$alive" = "0" ] && break
    sleep 0.5
done
kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
wait "$S1" 2>/dev/null || true
wait "$S2" 2>/dev/null || true
wait "$S3" 2>/dev/null || true
S1=""; S2=""; S3=""

sleep 2
cp -f "$LOCAL/server.perf.data" "$PROF/server.perf.data" 2>/dev/null || true
cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
ls -la "$PROF"

perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/client.perf.data" \
    > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || echo "client report failed"
perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/server.perf.data" \
    > "$PROF/server.report.txt" 2>"$PROF/server.report.err" || echo "server report failed"
perf report --stdio --percent-limit 0.5 -i "$PROF/client.perf.data" \
    > "$PROF/client.callers.txt" 2>/dev/null || true
perf report --stdio --percent-limit 0.5 -i "$PROF/server.perf.data" \
    > "$PROF/server.callers.txt" 2>/dev/null || true

ln -sfn "$PROF" "$SHARED/profile/latest"
ln -sfn "$PROF" "$SHARED/profile/heavy-latest"

echo "=== client report (top 50) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -50 || true
echo "=== server report (top 50) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/server.report.txt" 2>/dev/null | head -50 || true
echo "Heavy profile round $ROUND done: $PROF"
