#!/bin/bash
#SBATCH --job-name=efs-smoke-metaslot
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metaslot-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-metaslot-%j.err
#
# Dual-slot meta durability:
#   1) write + flush (live gen mountable)
#   2) abort mid next flush (pages written to inactive slot; root not flipped)
#   3) remount must still succeed with prior gen
#   4) complete a flush and remount on the new gen

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-metaslot-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1981
P2=1982
P3=1983
MNT="$LOCAL/mnt"
FAIL=0
EXPORT="metaslot"

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

echo "smoke-meta-dual-slot on $(hostname) LOCAL=$LOCAL"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    >"$OUT/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    grep -q "listening on" "$OUT/s1.stdout" 2>/dev/null && break
    kill -0 "$S1" 2>/dev/null || { echo "s1 died"; cat "$OUT/s1.stdout"; exit 1; }
    sleep 0.25
done
grep -q "listening on" "$OUT/s1.stdout" || { echo "s1 not ready"; cat "$OUT/s1.stdout"; exit 1; }

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    >"$OUT/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    >"$OUT/s3.stdout" 2>&1 &
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

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" "$EXPORT" 2>&1)"

mount_fuse() {
    local log=$1
    shift
    # Optional KEY=VAL args before the binary are applied to this fuse process only.
    EFS_META_BATCH_OPS=1 env "$@" \
        "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f \
        >"$log" 2>&1 &
    CPID=$!
    for i in $(seq 1 40); do
        mountpoint -q "$MNT" 2>/dev/null && return 0
        kill -0 "$CPID" 2>/dev/null || { echo "fuse died:"; cat "$log"; return 1; }
        sleep 0.25
    done
    echo "mount timeout"; cat "$log"; return 1
}

umount_fuse() {
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    if [ -n "${CPID:-}" ]; then
        wait "$CPID" 2>/dev/null || true
        CPID=""
    fi
    sleep 0.3
}

mount_fuse "$OUT/c1.stdout" || exit 1
mkdir -p "$MNT/d"
echo "gen1-data" >"$MNT/d/a.txt"
# release forces meta flush (batch=1)
umount_fuse
pass "initial write+flush"

mount_fuse "$OUT/c2.stdout" || { fail "remount after first flush"; exit 1; }
CONTENT=$(timeout 10 cat "$MNT/d/a.txt" || true)
[ "$CONTENT" = "gen1-data" ] && pass "read after first flush" || fail "read1 got '$CONTENT'"

umount_fuse

# Mid-flush abort: fuse process must inherit the hook env at start.
mount_fuse "$OUT/c2b.stdout" EFS_META_FLUSH_ABORT_AFTER_PAGES=1 || {
    fail "mount for abort session"; cat "$OUT/c2b.stdout"; exit 1
}
echo "will-abort" >"$MNT/d/b.txt"
# release/unmount triggers replicate: write page 0 to new slot, then abort
# before EFSR flip (1-page blob → abort_after=1).
umount_fuse
grep -q "EFS_META_FLUSH_ABORT_AFTER_PAGES" "$OUT/c2b.stdout" 2>/dev/null \
    && pass "abort hook fired" \
    || fail "abort hook did not log (see c2b.stdout)"

# Prior generation must still mount.
mount_fuse "$OUT/c3.stdout" || { fail "remount after aborted flush"; cat "$OUT/c3.stdout"; exit 1; }
CONTENT=$(timeout 10 cat "$MNT/d/a.txt" || true)
[ "$CONTENT" = "gen1-data" ] && pass "prior gen readable after abort" || fail "read after abort got '$CONTENT'"
# b.txt may or may not be in prior gen depending on whether create flushed before abort.
# The critical check is mount + a.txt.

# Complete a clean flush and remount.
echo "gen2-data" >"$MNT/d/c.txt"
umount_fuse
mount_fuse "$OUT/c4.stdout" || { fail "remount after clean flush"; exit 1; }
CONTENT=$(timeout 10 cat "$MNT/d/c.txt" || true)
[ "$CONTENT" = "gen2-data" ] && pass "new gen readable after clean flush" || fail "read gen2 got '$CONTENT'"
CONTENT=$(timeout 10 cat "$MNT/d/a.txt" || true)
[ "$CONTENT" = "gen1-data" ] && pass "old file still readable" || fail "a.txt after gen2 got '$CONTENT'"

umount_fuse

if [ "$FAIL" -ne 0 ]; then
    echo "SMOKE_FAIL"
    exit 1
fi
echo "SMOKE_OK"
exit 0
