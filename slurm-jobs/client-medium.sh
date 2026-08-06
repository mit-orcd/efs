#!/bin/bash
#SBATCH --job-name=efs-med-cli
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-medium-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-medium-%j.err
#
# Multi-node medium load client: connects to servers over InfiniBand
# (addresses in state/*.addr are hostname.ib:port).

set -euo pipefail
if [ -z "${EFS_MED_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_MED_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
PROF="${PROF_DIR:-$SHARED/profile/medium-ib-r${ROUND}-${SLURM_JOB_ID}}"
mkdir -p "$PROF" "$SHARED/logs"

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
echo "=== medium IB client on $(hostname -s) IB=$CLIENT_IB ($CLIENT_IP) ==="
echo "PROF=$PROF"

NFILES=50000
NDIRS=100
FILE_BYTES=65536
WRITERS=4

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-} ${SPERF:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in ${CPID:-} ${SPERF:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    # when the harness finishes, always cleanup after yourself in /scratch.
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

for i in 1 2 3; do
    WAITED=0
    while [ ! -f "$SHARED/state/s${i}.addr" ]; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 300 ]; then
            echo "Timed out waiting for s${i}.addr"
            exit 1
        fi
    done
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
            echo "Timed out waiting for $addr over IB"
            exit 1
        fi
    done
    echo "reachable: $addr"
done

MKFS_OUT=$("$REPO/efs-mgmt" mkfs "$S1" profexport 2>&1) || true
echo "mkfs: $MKFS_OUT"
echo "$MKFS_OUT" | grep -q "created\|already exists" || { echo "mkfs failed"; exit 1; }

EFS_META_BATCH_OPS=4096 \
EFS_PERF_PATH="$LOCAL/client.perf.data" \
    "$REPO/efs-fuse" "$S1" "$S2" "$S3" profexport "$MNT" -f --perf \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 60); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "client died:"; cat "$PROF/client.stdout"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/client.stdout"; exit 1; }
echo "mounted on $MNT via IB"

echo "=== load: ${NFILES} files x ${FILE_BYTES} bytes (~3 GiB), ${WRITERS} writers ==="
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
        dd if=/dev/zero of="$buf" bs="$FILE_BYTES" count=1 status=none
        n=0
        for ((d=d0; d<d1; d++)); do
            dir=$(printf "%s/tree/d%03d" "$MNT" "$d")
            mkdir -p "$dir"
            for ((f=0; f<files_per_dir; f++)); do
                path=$(printf "%s/f%06d" "$dir" "$f")
                cp "$buf" "$path"
                n=$((n + 1))
                if (( n % 1000 == 0 )); then
                    echo "[writer-$w] $n files ($(date +%H:%M:%S))"
                fi
            done
        done
        echo "[writer-$w] done $n files"
    ) > "$PROF/writer-$w.log" 2>&1 &
    WPIDS+=($!)
done
for pid in "${WPIDS[@]}"; do
    wait "$pid" || true
done
END=$(date +%s)
echo "write_wall_sec=$((END - START))"

echo "=== sample listing ==="
ls "$MNT/tree" | wc -l
ls "$MNT/tree/d000" | wc -l

echo "=== load: sequential read of 100 sample files ==="
START=$(date +%s)
for d in 0 25 50 75; do
    dir=$(printf "%s/tree/d%03d" "$MNT" "$d")
    for f in 0 100 200 300 400; do
        path=$(printf "%s/f%06d" "$dir" "$f")
        dd if="$path" of=/dev/null bs=64K status=none
    done
done
END=$(date +%s)
echo "sample_read_wall_sec=$((END - START))"
echo "=== load done ==="

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

sleep 2
cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
# Copy server addrs used for this run
cp -f "$SHARED/state"/s*.addr "$PROF/" 2>/dev/null || true
ls -la "$PROF"

if [ -f "$PROF/client.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/client.perf.data" \
        > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || echo "client report failed"
fi

{
    echo "client_ib=$CLIENT_IB"
    echo "servers=$S1 $S2 $S3"
    grep -E 'write_wall_sec|sample_read' "$PROF/../" 2>/dev/null || true
} > "$PROF/IB.txt" || true
echo "client_ib=$CLIENT_IB" > "$PROF/IB.txt"
echo "servers=$S1 $S2 $S3" >> "$PROF/IB.txt"

ln -sfn "$PROF" "$SHARED/profile/latest"
ln -sfn "$PROF" "$SHARED/profile/medium-ib-latest"

echo "=== client report (top 40) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -40 || true
echo "Medium IB client done: $PROF"
