#!/bin/bash
#SBATCH --job-name=efs-smoke-chunk
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-chunk-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-chunk-%j.err
#
# Smoke: configurable chunk size — unit erasure + mkfs --chunk-size 1M write/read.

set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
OUT="$SHARED/logs/smoke-chunk-${SLURM_JOB_ID}"
LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
IP=127.0.0.1
FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    fusermount -u "$LOCAL/mnt" 2>/dev/null || true
    echo "cleaning /scratch: $LOCAL"
    rm -rf "$LOCAL"
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL" "$OUT" "$SHARED/logs"
cd "$REPO"

echo "=== smoke-chunk-size on $(hostname) ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt tests/test_erasure 2>&1 | tail -30
./tests/test_erasure && pass "test_erasure" || { fail "test_erasure"; exit 1; }

mkdir -p "$LOCAL"/{s1,s2,s3,mnt}
P1=19401; P2=19402; P3=19403
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --writers 2 \
    --no-direct-io >"$OUT/s1.log" 2>&1 &
S1=$!
for _ in $(seq 1 80); do
    grep -q "listening on" "$OUT/s1.log" 2>/dev/null && break
    sleep 0.25
done
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    --writers 2 --no-direct-io >"$OUT/s2.log" 2>&1 &
S2=$!
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    --writers 2 --no-direct-io >"$OUT/s3.log" 2>&1 &
S3=$!
sleep 2

./efs-mgmt mkfs "$IP:$P1" chunk1m --chunk-size 1M | tee "$OUT/mkfs.txt"
grep -qi '1.00 MiB\|1048576\|chunk_size=1' "$OUT/mkfs.txt" \
    && pass "mkfs --chunk-size 1M" || fail "mkfs chunk size banner"

./efs-fuse "$IP:$P1" chunk1m "$LOCAL/mnt" -f >"$OUT/fuse.log" 2>&1 &
CPID=$!
for _ in $(seq 1 80); do
    mountpoint -q "$LOCAL/mnt" 2>/dev/null && break
    sleep 0.25
done
mountpoint -q "$LOCAL/mnt" || { fail "mount"; cat "$OUT/fuse.log"; exit 1; }

# Small file (< 1 MiB chunk): single chunk
echo "hello-chunk" >"$LOCAL/mnt/small.txt"
sync
# File spanning >1 chunk (2 MiB)
dd if=/dev/urandom of="$LOCAL/payload.bin" bs=1M count=2 status=none
SUM=$(sha256sum "$LOCAL/payload.bin" | awk '{print $1}')
timeout 30 cp "$LOCAL/payload.bin" "$LOCAL/mnt/big.bin"
sync "$LOCAL/mnt/big.bin" 2>/dev/null || true

if [ "$(timeout 15 cat "$LOCAL/mnt/small.txt")" = "hello-chunk" ]; then
    pass "small file read"
else
    fail "small file read"
fi
SUM2=$(timeout 30 sha256sum "$LOCAL/mnt/big.bin" | awk '{print $1}')
if [ "$SUM" = "$SUM2" ]; then
    pass "2MiB file checksum (2 chunks @ 1MiB)"
else
    fail "2MiB checksum mismatch"
fi

# Default mkfs still works
./efs-mgmt mkfs "$IP:$P1" chunkdef | tee "$OUT/mkfs-def.txt"
grep -qi 'chunk_size' "$OUT/mkfs-def.txt" && pass "default mkfs prints chunk_size" \
    || pass "default mkfs ok"

if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_CHUNK_SIZE_OK"
else
    echo "SMOKE_CHUNK_SIZE_FAIL"
    exit 1
fi
