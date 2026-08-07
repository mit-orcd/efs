#!/bin/bash
#SBATCH --job-name=efs-smoke-hot
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-hotpath-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-hotpath-%j.err
#
# Hot-path smoke: volume I/O, dut parallel meta, chmod/chown, truncate/sparse,
# concurrent same-mount + dual-mount access to one export.

set -uo pipefail
# Intentionally not -e: pass/fail tracks soft check failures; hard errors exit explicitly.

REPO="/home/erbmi1/git/efs"
DUT_SRC="/home/erbmi1/git/dut"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-hotpath-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=$((1900 + SLURM_JOB_ID % 700))
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
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$MNT2" "$OUT"

echo "smoke-hotpath on $(hostname) LOCAL=$LOCAL"

echo "=== build dut ==="
make -C "$DUT_SRC" -j"${SLURM_CPUS_PER_TASK:-2}"
DUT="$DUT_SRC/dut"
test -x "$DUT"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --writers 4 \
    > "$OUT/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    grep -q "listening on" "$OUT/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { cat "$OUT/s1.stdout"; exit 1; }
    sleep 0.25
done
"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" --writers 4 \
    > "$OUT/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" --writers 4 \
    > "$OUT/s3.stdout" 2>&1 &
S3=$!
sleep 2

"$REPO/efs-mgmt" mkfs "$IP:$P1" hotpath || { fail "mkfs"; exit 1; }

mount_one() {
    local mnt=$1
    local log=$2
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" hotpath "$mnt" -f \
        > "$log" 2>&1 &
    echo $!
}

CPID=$(mount_one "$MNT" "$OUT/fuse1.stdout")
for i in $(seq 1 40); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { cat "$OUT/fuse1.stdout"; exit 1; }
    sleep 0.25
done
mountpoint -q "$MNT" || { fail "mount1"; exit 1; }

CPID2=$(mount_one "$MNT2" "$OUT/fuse2.stdout")
for i in $(seq 1 40); do
    mountpoint -q "$MNT2" 2>/dev/null && break
    kill -0 "$CPID2" 2>/dev/null || { cat "$OUT/fuse2.stdout"; exit 1; }
    sleep 0.25
done
mountpoint -q "$MNT2" || { fail "mount2"; exit 1; }
pass "dual mount up"

HOTPATH_TMP="$LOCAL"
# shellcheck source=lib-hotpath-smoke.sh
source "$REPO/slurm-jobs/lib-hotpath-smoke.sh"
efs_hotpath_run_all "$MNT" "$MNT2"

if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_HOTPATH_OK"
else
    echo "SMOKE_HOTPATH_FAIL"
    exit 1
fi
