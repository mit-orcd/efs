#!/bin/bash
#SBATCH --job-name=efs-prof-hot
#SBATCH --partition=mit_quicktest,mit_normal
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-hotpath-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-hotpath-%j.err
#
# perf C profile of client (+ server1) during hot-path smoke.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
DUT_SRC="/home/erbmi1/git/dut"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PROF="$SHARED/profile/hotpath-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs" "$LOCAL"

IP="127.0.0.1"
P1=$((1950 + SLURM_JOB_ID % 700))
P2=$((P1 + 1))
P3=$((P1 + 2))
MNT="$LOCAL/mnt"
MNT2="$LOCAL/mnt2"
FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    fusermount -u "$MNT2" 2>/dev/null || umount "$MNT2" 2>/dev/null || true
    for pid in ${CPID:-} ${CPID2:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${CPID2:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    for f in client.perf.data server.perf.data; do
        [ -f "$LOCAL/$f" ] && cp -f "$LOCAL/$f" "$PROF/$f" 2>/dev/null || true
    done
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$MNT2" "$PROF"

echo "=== profile-hotpath on $(hostname) ==="
make -C "$DUT_SRC" -j"${SLURM_CPUS_PER_TASK:-2}"
DUT="$DUT_SRC/dut"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

EFS_PERF_PATH="$LOCAL/server.perf.data" \
    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    --writers 4 --perf > "$PROF/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    grep -q "listening on" "$PROF/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { cat "$PROF/s1.stdout"; exit 1; }
    sleep 0.25
done
"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" \
    --join "$IP:$P1" --writers 4 > "$PROF/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" \
    --join "$IP:$P1" --writers 4 > "$PROF/s3.stdout" 2>&1 &
S3=$!
sleep 2

"$REPO/efs-mgmt" mkfs "$IP:$P1" profhot

EFS_PERF_PATH="$LOCAL/client.perf.data" \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" profhot "$MNT" -f --perf \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 40); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { cat "$PROF/client.stdout"; exit 1; }
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "mount failed"; exit 1; }

"$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" profhot "$MNT2" -f \
    > "$PROF/client2.stdout" 2>&1 &
CPID2=$!
for i in $(seq 1 40); do
    mountpoint -q "$MNT2" 2>/dev/null && break
    sleep 0.25
done
mountpoint -q "$MNT2" || { echo "mount2 failed"; exit 1; }

HOTPATH_TMP="$LOCAL"
# shellcheck source=lib-hotpath-smoke.sh
source "$REPO/slurm-jobs/lib-hotpath-smoke.sh"
{
    efs_hotpath_run_all "$MNT" "$MNT2"
} | tee "$PROF/load.txt"

fusermount -u "$MNT2" 2>/dev/null || true
fusermount -u "$MNT" 2>/dev/null || true
for i in $(seq 1 40); do
    kill -0 "$CPID" 2>/dev/null || break
    sleep 0.25
done
kill -TERM "$CPID" 2>/dev/null || true
sleep 2
kill -KILL "$CPID" "$CPID2" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
wait "$CPID2" 2>/dev/null || true
CPID=""; CPID2=""
kill -INT "$S1" "$S2" "$S3" 2>/dev/null || true
sleep 2
kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
wait "$S1" "$S2" "$S3" 2>/dev/null || true
S1=""; S2=""; S3=""

sleep 1
cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
cp -f "$LOCAL/server.perf.data" "$PROF/server.perf.data" 2>/dev/null || true

if [ -f "$PROF/client.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/client.perf.data" \
        > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || true
    perf report --stdio --percent-limit 0.8 -i "$PROF/client.perf.data" \
        > "$PROF/client.callers.txt" 2>/dev/null || true
fi
if [ -f "$PROF/server.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/server.perf.data" \
        > "$PROF/server.report.txt" 2>"$PROF/server.report.err" || true
fi

ln -sfn "$PROF" "$SHARED/profile/hotpath-latest"
ln -sfn "$PROF" "$SHARED/profile/latest"

echo "=== timings ==="
grep -E '_real=' "$PROF/load.txt" || true
echo "=== client top ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -40 || true
echo "=== server top ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/server.report.txt" 2>/dev/null | head -40 || true
echo "=== efs/fuse/meta focus (client) ==="
grep -E 'fuse|efs_|lookup|readdir|getattr|utimens|chmod|chown|truncate|blake3|hash|put_frag|replicate|dut' \
    "$PROF/client.report.txt" 2>/dev/null | head -50 || true

if [ "$FAIL" -ne 0 ]; then
    echo "PROFILE_HOTPATH_FAIL"
    exit 1
fi
echo "PROFILE_HOTPATH_OK $PROF"
