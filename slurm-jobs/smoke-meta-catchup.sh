#!/bin/bash
#SBATCH --job-name=efs-smoke-metacatch
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metacatch-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metacatch-%j.err
#
# Kill s2 mid-cluster, advance meta on survivors, rejoin s2, wait for
# meta-heal / meta-catchup, then verify files via query-stats + remount read.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-metacatch-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1991
P2=1992
P3=1993
MNT="$LOCAL/mnt"
FAIL=0
EXPORT="catchup1"

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
cd "$REPO"

echo "smoke-meta-catchup on $(hostname) LOCAL=$LOCAL"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

echo "=== build ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt efs-query

echo "=== start 3 servers ==="
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --writers 4 \
    >"$OUT/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    grep -q "listening on" "$OUT/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { echo "s1 died"; cat "$OUT/s1.stdout"; exit 1; }
    sleep 0.25
done
grep -q "listening on" "$OUT/s1.stdout" || { echo "s1 not ready"; cat "$OUT/s1.stdout"; exit 1; }

./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --writers 4 \
    --join "$IP:$P1" >"$OUT/s2.stdout" 2>&1 &
S2=$!
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --writers 4 \
    --join "$IP:$P1" >"$OUT/s3.stdout" 2>&1 &
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

./efs-mgmt mkfs "$IP:$P1" "$EXPORT"

echo "=== mount + initial writes ==="
EFS_META_BATCH_OPS=1 \
    ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f \
    >"$OUT/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 40); do
    mountpoint -q "$MNT" && break
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$OUT/client.stdout"; exit 1; }

echo "pre-down" >"$MNT/a.txt"
for i in $(seq 1 20); do
    echo "file-$i" >"$MNT/f$i.txt"
done
sync
sleep 0.5

echo "=== kill s2 ==="
kill -KILL "$S2" 2>/dev/null || true
wait "$S2" 2>/dev/null || true
S2=
sleep 1

echo "=== writes while s2 down ==="
echo "while-down" >"$MNT/b.txt"
for i in $(seq 21 40); do
    echo "file-$i" >"$MNT/f$i.txt"
done
sync
# Force attribute/meta flush via unmount
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=
sleep 1

echo "=== restart s2 with --join ==="
: >"$OUT/s2.stdout"
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --writers 4 \
    --join "$IP:$P1" >"$OUT/s2.stdout" 2>&1 &
S2=$!
for i in $(seq 1 40); do
    if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$P2" 2>/dev/null; then break; fi
    sleep 0.25
done
timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$P2" 2>/dev/null || {
    echo "s2 failed to listen"; cat "$OUT/s2.stdout"; exit 1;
}

echo "=== wait for meta-heal / meta-catchup on s2 ==="
HEALED=0
for i in $(seq 1 60); do
    if grep -qE 'meta-heal:|meta-catchup: rebuilt' "$OUT/s2.stdout" 2>/dev/null; then
        HEALED=1
        break
    fi
    sleep 0.5
done

if [ "$HEALED" = "1" ]; then
    pass "s2 meta catch-up/heal observed"
else
    echo "--- s2 log ---"
    cat "$OUT/s2.stdout" || true
    fail "s2 did not log meta-heal or meta-catchup rebuild"
fi

timeout 10 ./efs-query "$IP:$P2" | tee "$OUT/stats-s2.txt" \
    && pass "efs-query via s2" \
    || fail "efs-query via s2"

echo "=== remount and verify ==="
EFS_META_BATCH_OPS=1 \
    ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f \
    >"$OUT/client2.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 40); do
    mountpoint -q "$MNT" && break
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "remount failed"; cat "$OUT/client2.stdout"; exit 1; }

[ "$(cat "$MNT/a.txt")" = "pre-down" ] && pass "a.txt" || fail "a.txt"
[ "$(cat "$MNT/b.txt")" = "while-down" ] && pass "b.txt" || fail "b.txt"
[ "$(cat "$MNT/f40.txt")" = "file-40" ] && pass "f40.txt" || fail "f40.txt"

if [ "$FAIL" -ne 0 ]; then
    echo "SMOKE_FAIL"
    exit 1
fi
echo "SMOKE_OK"
exit 0
