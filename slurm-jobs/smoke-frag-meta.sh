#!/bin/bash
#SBATCH --job-name=efs-smoke-fragmeta
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-fragmeta-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-fragmeta-%j.err
#
# Smoke: hybrid fragmented metadata — write namespace + file, kill one server,
# remount via remaining nodes, verify meta and data.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-fragmeta-${SLURM_JOB_ID}"
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

echo "smoke-fragmeta on $(hostname) LOCAL=$LOCAL"

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

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" fragmeta 2>&1)"

# Flush meta every op so EFSR + pages are durable before we kill a node.
EFS_META_BATCH_OPS=1 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" fragmeta "$MNT" -f \
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

mkdir -p "$MNT/dir"
echo "hello-frag-meta" > "$MNT/dir/file.txt"
sync
# Force release/flush
sleep 0.5

# Confirm root is fragmented (EFSR) on at least one server.
MAGIC=$(head -c 4 "$LOCAL/s1/meta/exports/fragmeta/metadata.bin" 2>/dev/null || true)
if [ "$MAGIC" = "EFSR" ]; then
    pass "metadata.bin is EFSR root"
else
    fail "metadata.bin is not EFSR root"
    xxd "$LOCAL/s1/meta/exports/fragmeta/metadata.bin" 2>/dev/null | head -3 || true
fi

# Meta table fragments use sharded inode dirs (5×0000-9999), e.g.
# .../exports/<id>/0922/3372/0368/5477/5810/<bucket>/...
META_SHARD="0922/3372/0368/5477/5810"
META_FRAGS=$(find "$LOCAL"/s*/data/exports -type f ! -name '*.sum' 2>/dev/null \
    | grep -c "$META_SHARD" || true)
# User file fragments must also land under 0000/... sharding (not flat {ino}/).
SHARDED=$(find "$LOCAL"/s*/data/exports -type f ! -name '*.sum' 2>/dev/null \
    | grep -E '/[0-9]{4}/[0-9]{4}/[0-9]{4}/[0-9]{4}/[0-9]{4}/' | wc -l || true)
if [ "${META_FRAGS:-0}" -ge 2 ]; then
    pass "found $META_FRAGS meta-table fragment files under sharded path"
elif [ "${SHARDED:-0}" -ge 3 ]; then
    pass "found $SHARDED sharded fragment files under data/exports"
else
    fail "expected sharded fragments (meta=$META_FRAGS sharded=$SHARDED)"
    find "$LOCAL"/s*/data/exports -type f 2>/dev/null | head -20 || true
fi
if [ "${SHARDED:-0}" -ge 1 ]; then
    pass "inode path sharding present ($SHARDED files)"
else
    fail "no 0000/0000/0000/0000/0000-style shard paths found"
fi

CONTENT=$(timeout 10 cat "$MNT/dir/file.txt" || true)
[ "$CONTENT" = "hello-frag-meta" ] && pass "read before kill" || fail "read before kill got '$CONTENT'"

# Unmount, kill server 2, remount via s1+s3 only.
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""
sleep 0.5

kill -KILL "$S2" 2>/dev/null || true
wait "$S2" 2>/dev/null || true
S2=""
sleep 0.5

# Keep all three addresses so placement matches the live cluster even though
# s2 is down (2+1 can still reconstruct from s1+s3).
EFS_META_BATCH_OPS=1 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" fragmeta "$MNT" -f \
    > "$OUT/client2.stdout" 2>&1 &
CPID=$!

MOUNTED=0
for i in $(seq 1 40); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "remount client died:"; cat "$OUT/client2.stdout"; exit 1
    fi
    sleep 0.25
done
[ "$MOUNTED" = "1" ] || { echo "remount failed"; cat "$OUT/client2.stdout"; exit 1; }

CONTENT=$(timeout 10 cat "$MNT/dir/file.txt" || true)
[ "$CONTENT" = "hello-frag-meta" ] && pass "read after killing s2" || fail "read after kill got '$CONTENT'"

[ -d "$MNT/dir" ] && pass "directory visible after kill" || fail "directory missing after kill"

if [ "$FAIL" -ne 0 ]; then
    echo "SMOKE_FAIL"
    exit 1
fi
echo "SMOKE_OK"
exit 0
