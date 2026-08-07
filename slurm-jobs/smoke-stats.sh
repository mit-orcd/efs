#!/bin/bash
#SBATCH --job-name=efs-smoke-stats
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-stats-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-stats-%j.err
#
# Smoke: virtual .stats rollup file (cat, read-only, reserved name).

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-stats-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1981
P2=1982
P3=1983
MNT="$LOCAL/mnt"
FAIL=0

pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT"

echo "smoke-stats on $(hostname) LOCAL=$LOCAL"

echo "building (clean; efs_inode/export layout changed)..."
make -C "$REPO" clean > "$OUT/build.log" 2>&1 || true
make -C "$REPO" -j"$(nproc)" all >> "$OUT/build.log" 2>&1 || {
    echo "build failed"; tail -80 "$OUT/build.log"; exit 1
}

"$REPO/tests/test_dir_stats" > "$OUT/unit.out" 2>&1 || {
    echo "unit test failed:"; cat "$OUT/unit.out"; exit 1
}
pass "test_dir_stats"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    > "$OUT/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    if grep -q "listening on" "$OUT/s1.stdout" 2>/dev/null; then break; fi
    if ! kill -0 "$S1" 2>/dev/null; then
        echo "server 1 died:"; cat "$OUT/s1.stdout"; exit 1
    fi
    sleep 0.25
done
grep -q "listening on" "$OUT/s1.stdout" || { echo "s1 not ready"; cat "$OUT/s1.stdout"; exit 1; }

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    > "$OUT/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    > "$OUT/s3.stdout" 2>&1 &
S3=$!

for port in "$P2" "$P3"; do
    READY=0
    for i in $(seq 1 40); do
        if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then READY=1; break; fi
        sleep 0.25
    done
    [ "$READY" = "1" ] || { echo "server :$port failed"; cat "$OUT"/s*.stdout; exit 1; }
done
sleep 0.5

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" statsmoke 2>&1)"

EFS_META_BATCH_OPS=1 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" statsmoke "$MNT" -f \
    > "$OUT/client.stdout" 2>&1 &
CPID=$!

MOUNTED=0
for i in $(seq 1 40); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "client died:"; cat "$OUT/client.stdout"; exit 1
    fi
    sleep 0.25
done
[ "$MOUNTED" = "1" ] || { echo "mount failed"; cat "$OUT/client.stdout"; exit 1; }
echo "mounted"

timeout 15 mkdir -p "$MNT/a/b" || fail "mkdir"
timeout 15 bash -c "echo -n hello > '$MNT/a/f1'" || fail "create f1"
timeout 15 bash -c "echo -n world!! > '$MNT/a/b/f2'" || fail "create f2"

STATS=$(timeout 15 cat "$MNT/a/.stats" 2>/dev/null || true)
echo "a/.stats:"
echo "$STATS"
echo "$STATS" | grep -q '^imm_files=1$' && pass "a imm_files=1" || fail "a imm_files"
echo "$STATS" | grep -q '^imm_dirs=1$' && pass "a imm_dirs=1" || fail "a imm_dirs"
echo "$STATS" | grep -q '^tree_files=2$' && pass "a tree_files=2" || fail "a tree_files"
echo "$STATS" | grep -q '^tree_dirs=1$' && pass "a tree_dirs=1" || fail "a tree_dirs"
echo "$STATS" | grep -q '^imm_bytes=5$' && pass "a imm_bytes=5" || fail "a imm_bytes"
echo "$STATS" | grep -q '^tree_bytes=12$' && pass "a tree_bytes=12" || fail "a tree_bytes"

ROOTS=$(timeout 15 cat "$MNT/.stats" 2>/dev/null || true)
echo "$ROOTS" | grep -q '^tree_files=2$' && pass "root tree_files=2" || fail "root tree_files"
echo "$ROOTS" | grep -q '^tree_bytes=12$' && pass "root tree_bytes=12" || fail "root tree_bytes"

if timeout 5 bash -c "echo x > '$MNT/a/.stats'" 2>/dev/null; then
    fail "write .stats should fail"
else
    pass "write .stats rejected"
fi

if timeout 5 bash -c "touch '$MNT/a/.stats'" 2>/dev/null; then
    fail "create real .stats should fail"
else
    pass "create .stats rejected"
fi

timeout 15 ls -a "$MNT/a" | grep -qx '\.stats' && pass "ls -a shows .stats" || fail "ls -a .stats"

if [ "$FAIL" -ne 0 ]; then
    echo "smoke-stats: FAILED"
    exit 1
fi
echo "smoke-stats: OK"
exit 0
