#!/bin/bash
#SBATCH --job-name=efs-smoke-restart
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-restart-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-restart-%j.err
#
# Smoke: write on a live FUSE mount, restart all three efsd processes (same
# storage / addr / port), then write+read again without unmounting.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-restart-${SLURM_JOB_ID}"
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
        kill -KILL "$pid" 2>/dev/null || true
    done
    sleep 0.5
    rm -rf "$LOCAL"
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT"
cd "$REPO"

echo "Building..."
make -j2 efsd efs-fuse efs-mgmt >/dev/null

start_servers() {
    ./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
        >"$OUT/s1.stdout" 2>&1 &
    S1=$!
    ./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" \
        >"$OUT/s2.stdout" 2>&1 &
    S2=$!
    ./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" \
        >"$OUT/s3.stdout" 2>&1 &
    S3=$!
    for port in "$P1" "$P2" "$P3"; do
        READY=0
        for i in $(seq 1 40); do
            if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then
                READY=1
                break
            fi
            sleep 0.25
        done
        [ "$READY" = "1" ] || { echo "server :$port not ready"; exit 1; }
    done
}

echo "Starting servers..."
start_servers
sleep 0.5
./efs-mgmt add-node "$IP:$P2" "$IP:$P1"
./efs-mgmt add-node "$IP:$P3" "$IP:$P1"
./efs-mgmt mkfs "$IP:$P1" restarttest

EFS_META_BATCH_OPS=64 \
    ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" restarttest "$MNT" -f \
    >"$OUT/client.stdout" 2>&1 &
CPID=$!
for i in $(seq 1 40); do
    mountpoint -q "$MNT" && break
    sleep 0.25
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$OUT/client.stdout"; exit 1; }

echo "before-restart" >"$MNT/a.txt"
dd if=/dev/zero of="$MNT/pre.bin" bs=64K count=16 status=none
sync
sleep 0.3
[ "$(cat "$MNT/a.txt")" = "before-restart" ] && pass "read before restart" \
    || fail "read before restart"

echo "Restarting all servers (client stays mounted)..."
kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
wait "$S1" "$S2" "$S3" 2>/dev/null || true
S1=; S2=; S3=
sleep 1
start_servers
# Persisted peers should rejoin; give HELLO a moment.
sleep 2

echo "after-restart" >"$MNT/b.txt" \
    && pass "create after restart" \
    || fail "create after restart"
dd if=/dev/zero of="$MNT/post.bin" bs=64K count=32 status=none \
    && pass "dd after restart" \
    || fail "dd after restart"
[ "$(cat "$MNT/a.txt")" = "before-restart" ] && pass "old file still readable" \
    || fail "old file unreadable"
[ "$(cat "$MNT/b.txt")" = "after-restart" ] && pass "new file readable" \
    || fail "new file unreadable"
sz=$(stat -c %s "$MNT/post.bin" 2>/dev/null || echo 0)
[ "$sz" = "2097152" ] && pass "post.bin size $sz" || fail "post.bin size $sz"

if [ "$FAIL" -ne 0 ]; then
    echo "==== client log ===="
    tail -40 "$OUT/client.stdout" || true
    echo "SMOKE_FAIL"
    exit 1
fi
echo "SMOKE_OK"
exit 0
