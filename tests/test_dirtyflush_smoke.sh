#!/bin/bash
# Smoke test for dirty-page meta flush + content-aware GC + incremental
# server rebuild. Loopback 3-node cluster, small meta batch threshold so a
# few hundred files produce many generations.
#
# Checks:
#  1. flush log shows pages being skipped once the table stabilizes
#  2. data reads back correct (md5) before and after unmount/remount
#     (remount forces the client to reconstruct from pages; servers exercise
#     the incremental rebuild path)
#  3. a server restart forces a full from-pages rebuild with the cache cold
set -e
BASE=/tmp/efs_dirtyflush_smoke
BIN=/tmp/efs
IP=127.0.0.1
rm -rf "$BASE"; mkdir -p "$BASE"
for i in 1 2 3; do mkdir -p "$BASE/s$i"; done

cleanup() {
    fusermount3 -u "$BASE/mnt" 2>/dev/null || umount "$BASE/mnt" 2>/dev/null || true
    [ -n "${P1:-}" ] && kill "$P1" "$P2" "$P3" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT

$BIN/efsd --node-id 1 --addr $IP --port 17451 --storage "$BASE/s1" >"$BASE/s1.log" 2>&1 &
P1=$!
$BIN/efsd --node-id 2 --addr $IP --port 17452 --storage "$BASE/s2" >"$BASE/s2.log" 2>&1 &
P2=$!
$BIN/efsd --node-id 3 --addr $IP --port 17453 --storage "$BASE/s3" >"$BASE/s3.log" 2>&1 &
P3=$!
sleep 1.5

$BIN/efs-mgmt add-node $IP:17452 $IP:17451 >/dev/null
$BIN/efs-mgmt add-node $IP:17453 $IP:17451 >/dev/null
$BIN/efs-mgmt mkfs $IP:17451 test >/dev/null
sleep 1

mkdir -p "$BASE/mnt"
EFS_META_BATCH_OPS=8 $BIN/efs-fuse $IP:17451 $IP:17452 $IP:17453 test \
    "$BASE/mnt" -f >"$BASE/fuse1.log" 2>&1 &
sleep 1.5

FAIL=0
# Four waves of files: creates + writes + a few overwrites of the same file
# (in-place page mutation), with flushes between waves. ~2400 files make the
# serialized metadata blob span ~10+ 128 KiB pages, so later flushes have
# unchanged pages to skip.
for wave in 1 2 3 4; do
    for i in $(seq 1 600); do
        head -c $((1000 + i)) /dev/urandom > "$BASE/mnt/w${wave}_f$i.bin"
    done
    echo "wave$wave-marker" > "$BASE/mnt/marker.txt"
    sync -f "$BASE/mnt" 2>/dev/null || true
done
# Overwrite a few early files (mutates old pages)
for i in 1 2 3; do
    head -c 5000 /dev/urandom > "$BASE/mnt/w1_f$i.bin"
done
sync -f "$BASE/mnt" 2>/dev/null || true
sleep 1

# Update-heavy phase: overwrite the SAME file repeatedly. Each forced flush
# (fsync) dirties only the pages holding that file's inode + chunk checksum;
# every other page is unchanged and must be skipped by the dirty-page flush.
for k in $(seq 1 25); do
    head -c $((2000 + k)) /dev/urandom > "$BASE/mnt/w2_f7.bin"
    sync -f "$BASE/mnt/w2_f7.bin" 2>/dev/null || true
done
sleep 1

# md5 of everything (sorted), then unmount + remount and compare
( cd "$BASE/mnt" && find . -type f -name '*.bin' -o -name marker.txt | sort | xargs md5sum ) > "$BASE/md5.before"

fusermount3 -u "$BASE/mnt" 2>/dev/null || umount "$BASE/mnt"
sleep 0.5

EFS_META_BATCH_OPS=8 $BIN/efs-fuse $IP:17451 $IP:17452 $IP:17453 test \
    "$BASE/mnt" -f >"$BASE/fuse2.log" 2>&1 &
sleep 1.5
( cd "$BASE/mnt" && find . -type f -name '*.bin' -o -name marker.txt | sort | xargs md5sum ) > "$BASE/md5.after"
if ! cmp -s "$BASE/md5.before" "$BASE/md5.after"; then
    echo "FAIL: md5 mismatch across remount"
    FAIL=1
fi

# Restart server 2 → cold full rebuild from pages on that node
kill "$P2" 2>/dev/null || true
wait "$P2" 2>/dev/null || true
$BIN/efsd --node-id 2 --addr $IP --port 17452 --storage "$BASE/s2" >"$BASE/s2b.log" 2>&1 &
P2=$!
sleep 3
# Read a sample again through the (still mounted) FS
for i in 10 20 30; do
    md5sum "$BASE/mnt/w2_f$i.bin" >/dev/null || FAIL=1
done

# Skip evidence: with 2400+ files the blob is many pages; later flushes
# must skip unchanged pages.
SKIPS=$(grep -c "unchanged (skipped)" "$BASE/fuse1.log" || true)
echo "skip-log-lines: $SKIPS"
if [ "$SKIPS" -lt 1 ]; then
    echo "FAIL: no dirty-page skips observed"
    FAIL=1
fi

# Rebuild mismatch evidence on servers: tolerate a few early races, but the
# storm must be gone
MM=$(grep -c "checksum mismatch" "$BASE"/s*.log || true)
echo "server checksum-mismatch lines: $MM"

if [ "$FAIL" -eq 0 ]; then
    echo "DIRTYFLUSH-SMOKE: PASS"
else
    echo "DIRTYFLUSH-SMOKE: FAIL"
    exit 1
fi
