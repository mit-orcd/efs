#!/bin/bash
#SBATCH --job-name=efs-smoke-rsync
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-rsync-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-rsync-%j.err
#
# Smoke: rsync -a twice to the same target; second pass must transfer 0 files.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-rsync-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1961
P2=1962
P3=1963
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
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT" "$LOCAL/src"

echo "smoke-rsync on $(hostname) LOCAL=$LOCAL"

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

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" rsyncsmoke 2>&1)"

EFS_META_BATCH_OPS=1 \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" rsyncsmoke "$MNT" -f \
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

# Small tree with varied mtimes (incl. nsec-ish via touch -d) and a .gitignore.
mkdir -p "$LOCAL/src/git/ereport" "$LOCAL/src/sub"
echo "hello" > "$LOCAL/src/a.txt"
echo "world" > "$LOCAL/src/sub/b.txt"
printf '*\n!*.c\n' > "$LOCAL/src/git/ereport/.gitignore"
dd if=/dev/urandom of="$LOCAL/src/sub/blob.bin" bs=1K count=4 status=none
touch -d "2020-01-02 03:04:05.123456789" "$LOCAL/src/a.txt"
touch -d "2021-06-15 12:00:00.987654321" "$LOCAL/src/git/ereport/.gitignore"

DST="$MNT/dst"
mkdir -p "$DST"

echo "=== rsync pass 1 ==="
timeout 60 rsync -a --info=stats2 "$LOCAL/src/" "$DST/" | tee "$OUT/rsync1.txt"
XFER1=$(grep -E 'Number of regular files transferred:' "$OUT/rsync1.txt" | awk '{print $NF}')
echo "pass1 transferred=$XFER1"
[ -n "$XFER1" ] && [ "$XFER1" -ge 1 ] && pass "pass1 transferred $XFER1 files" || fail "pass1 xfer=$XFER1"

SRC_FULL=$(find "$LOCAL/src/a.txt" -printf '%T@\n')
DST_FULL=$(find "$DST/a.txt" -printf '%T@\n')
echo "a.txt mtime src=$SRC_FULL dst=$DST_FULL"
awk -v s="$SRC_FULL" -v d="$DST_FULL" 'BEGIN {
  ds = s + 0; dd = d + 0;
  diff = ds - dd; if (diff < 0) diff = -diff;
  exit(diff < 1e-8 ? 0 : 1)
}' && pass "mtime preserved on a.txt" || fail "mtime not preserved src=$SRC_FULL dst=$DST_FULL"

echo "=== rsync pass 2 (should be idle) ==="
timeout 60 rsync -a --info=stats2 "$LOCAL/src/" "$DST/" | tee "$OUT/rsync2.txt"
# Also capture itemize for diagnosis
timeout 60 rsync -ani "$LOCAL/src/" "$DST/" | tee "$OUT/rsync2-itemize.txt" || true
XFER2=$(grep -E 'Number of regular files transferred:' "$OUT/rsync2.txt" | awk '{print $NF}')
echo "pass2 transferred=$XFER2"
if [ "$XFER2" = "0" ]; then
    pass "pass2 transferred 0 files"
else
    fail "pass2 re-transferred $XFER2 files (expected 0)"
    echo "--- itemize ---"
    cat "$OUT/rsync2-itemize.txt"
fi

# Content still matches
cmp "$LOCAL/src/a.txt" "$DST/a.txt" && pass "content a.txt" || fail "content a.txt"
cmp "$LOCAL/src/git/ereport/.gitignore" "$DST/git/ereport/.gitignore" \
    && pass "content .gitignore" || fail "content .gitignore"

echo "=== summary ==="
if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_RSYNC_OK"
    exit 0
fi
echo "SMOKE_RSYNC_FAIL"
exit 1
