#!/bin/bash
set -e

MNT=$1
CLIENT=$2
REPO="/home/erbmi1/git/efs"

log() {
    echo "[client-$CLIENT] $(date +%H:%M:%S) $*"
}

if ! mountpoint -q "$MNT"; then
    log "ERROR: $MNT is not a mountpoint"
    exit 1
fi

TEST_DIR="$MNT/client-${CLIENT}-test-$$"
mkdir -p "$TEST_DIR"
cd "$TEST_DIR"

# Fast timeout so a hung operation fails quickly instead of blocking the job.
T=15
run() {
    timeout "$T" "$@"
}

log "=== tiny write ==="
run tee tiny.txt <<< "hello client $CLIENT" >/dev/null
run cat tiny.txt

log "=== small dd ==="
run dd if=/dev/zero of=small.bin bs=1K count=10 status=none
run ls -lh small.bin

log "=== cp ==="
run cp small.bin cp.bin
run ls -lh cp.bin

log "=== mv ==="
run mv cp.bin mv.bin
run test -f mv.bin
run test ! -f cp.bin

log "=== sed / cat ==="
run tee text.txt <<< "hello world from client $CLIENT" >/dev/null
run sed -i 's/world/efs/' text.txt
run cat text.txt
run cp text.txt text-copy.txt
run diff text.txt text-copy.txt

log "=== append ==="
run tee -a text.txt <<< "appended line" >/dev/null
run wc -l text.txt

log "=== rsync ==="
mkdir -p rsync-src
for i in 1 2; do
    run dd if=/dev/urandom of="rsync-src/file-$i.bin" bs=10K count=1 status=none
done
mkdir -p rsync-dst
run rsync -a rsync-src/ rsync-dst/
for i in 1 2; do
    run cmp "rsync-src/file-$i.bin" "rsync-dst/file-$i.bin"
done
# Second pass must be a no-op (mtime/size preserved).
STATS=$(run rsync -a --info=stats2 rsync-src/ rsync-dst/)
echo "$STATS"
echo "$STATS" | grep -q 'Number of regular files transferred: 0' \
    || { log "ERROR: rsync second pass re-transferred files"; exit 1; }
log "rsync idempotent OK"

log "=== rclone ==="
if command -v ml >/dev/null 2>&1; then
    ml rclone 2>/dev/null || true
fi
if command -v rclone >/dev/null 2>&1; then
    mkdir -p rclone-dst
    run rclone sync rsync-src/ rclone-dst/ --transfers 2
    for i in 1 2; do
        run diff "rsync-src/file-$i.bin" "rclone-dst/file-$i.bin"
    done
    log "rclone sync OK"
else
    log "rclone not available, skipping"
fi

log "=== rm / rmdir ==="
run rm -f tiny.txt small.bin mv.bin text.txt text-copy.txt
run rm -rf rsync-src rsync-dst rclone-dst

cd "$MNT"
run rmdir "$TEST_DIR" 2>/dev/null || true

log "=== done ==="
