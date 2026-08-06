#!/bin/bash
# Client workload for the 4-node kill-s4 harness:
#   1) write a few 10 MiB files under node-local /scratch
#   2) checksum them
#   3) mv onto the efs mount (orchestrator kills s4 while this is in flight)
#   4) sync the mount
#   5) read back and compare checksums
set -euo pipefail

MNT=$1
CLIENT=$2
REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
LOCAL="$SCRATCH/local"
DEST="$MNT/client-${CLIENT}"
NFILES=${NFILES:-3}
FILE_MB=${FILE_MB:-10}

log() {
    echo "[client-$CLIENT] $(date +%H:%M:%S) $*"
}

run() {
    local t=$1
    shift
    timeout "$t" "$@"
}

if ! mountpoint -q "$MNT"; then
    log "ERROR: $MNT is not a mountpoint"
    exit 1
fi

mkdir -p "$LOCAL" "$DEST"

log "=== write ${NFILES} x ${FILE_MB}MiB files under $LOCAL ==="
for i in $(seq 1 "$NFILES"); do
    run 60 dd if=/dev/urandom of="$LOCAL/file-${i}.bin" bs=1M count="$FILE_MB" status=none
    run 15 ls -lh "$LOCAL/file-${i}.bin"
done

log "=== checksum originals ==="
(cd "$LOCAL" && sha256sum file-*.bin | sort) > "$SCRATCH/sums.orig"
cat "$SCRATCH/sums.orig"
cp "$SCRATCH/sums.orig" "$SHARED/state/c${CLIENT}.sums.orig"

log "=== ready; waiting for orchestrator go.copy ==="
touch "$SHARED/state/c${CLIENT}.ready"
WAITED=0
while [ ! -f "$SHARED/state/go.copy" ]; do
    sleep 1
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -ge 300 ]; then
        log "ERROR: timed out waiting for go.copy"
        exit 1
    fi
done

log "=== mv to efs mount (expect s4 kill in flight) ==="
touch "$SHARED/state/c${CLIENT}.copying"
PIDS=()
for i in $(seq 1 "$NFILES"); do
    # Cross-filesystem mv = copy + unlink; keep several in flight.
    mv "$LOCAL/file-${i}.bin" "$DEST/file-${i}.bin" &
    PIDS+=($!)
done
RC=0
for p in "${PIDS[@]}"; do
    if ! wait "$p"; then
        RC=1
    fi
done
if [ "$RC" != 0 ]; then
    log "ERROR: one or more mv operations failed (s4 may have died mid-write)"
    exit 1
fi
touch "$SHARED/state/c${CLIENT}.copied"
log "mv complete"

log "=== sync mount ==="
run 60 sync "$DEST"
run 60 sync
# Force kernel page-cache flush of the FUSE mount.
run 60 find "$DEST" -type f -exec sync {} \;
log "sync complete"
touch "$SHARED/state/c${CLIENT}.synced"

if [ -f "$SHARED/state/s4.killed" ]; then
    log "confirmed: s4 was killed during/after transfer"
else
    log "WARNING: s4.killed flag not set yet (kill may be slightly late)"
fi

log "=== read back and checksum ==="
(cd "$DEST" && sha256sum file-*.bin | sort) > "$SCRATCH/sums.new"
cat "$SCRATCH/sums.new"
cp "$SCRATCH/sums.new" "$SHARED/state/c${CLIENT}.sums.new"

if ! cmp -s "$SCRATCH/sums.orig" "$SCRATCH/sums.new"; then
    log "ERROR: checksum mismatch after kill-s4"
    log "--- original ---"
    cat "$SCRATCH/sums.orig"
    log "--- after read ---"
    cat "$SCRATCH/sums.new"
    exit 1
fi

log "checksums match OK"
touch "$SHARED/state/c${CLIENT}.ok"
log "=== done ==="
