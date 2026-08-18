#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 <server-addr:port> <mount-path> [export-name] [extra-efs-fuse-args...]
  $0 stop <mount-path>

  start:  mount export at mount-path (default export name: fs)
  stop:   unmount mount-path (fusermount3 -u, then umount)
EOF
    exit 1
}

# Run from the project root so efs-fuse is found.
cd "$(dirname "$0")/.."

# True if kernel still has this path mounted (works even when stat → ENOTCONN).
is_listed_mount() {
    local mnt=$1
    # Canonicalize for /proc/mounts compare when possible; fall back to literal.
    local abs=$mnt
    if command -v realpath >/dev/null 2>&1; then
        abs=$(realpath -m "$mnt" 2>/dev/null || echo "$mnt")
    fi
    awk -v m="$abs" -v m2="$mnt" '
        BEGIN { found = 0 }
        {
            # /proc/mounts escapes spaces as \040, etc.; compare raw for our paths.
            if ($2 == m || $2 == m2) found = 1
        }
        END { exit found ? 0 : 1 }
    ' /proc/mounts 2>/dev/null
}

cmd_stop() {
    if [ $# -lt 1 ]; then
        echo "Usage: $0 stop <mount-path>" >&2
        exit 1
    fi
    local mnt=$1

    # Do not trust [ -e ] / [ -d ]: a dead FUSE mount returns ENOTCONN and
    # those tests look like "does not exist" even though /proc/mounts lists it.
    local listed=0
    if is_listed_mount "$mnt" || mountpoint -q "$mnt" 2>/dev/null; then
        listed=1
    fi

    echo "Unmounting $mnt"
    fusermount3 -u "$mnt" 2>/dev/null || fusermount3 -uz "$mnt" 2>/dev/null || true
    umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true

    if is_listed_mount "$mnt" || mountpoint -q "$mnt" 2>/dev/null; then
        echo "ERROR: still mounted: $mnt" >&2
        echo "Try: fusermount3 -uz $mnt   or   umount -l $mnt" >&2
        exit 1
    fi

    if [ "$listed" -eq 1 ]; then
        echo "Unmounted $mnt"
    else
        echo "Not mounted: $mnt (cleared any stale FUSE handle)"
    fi
}

if [ $# -lt 1 ]; then
    usage
fi

if [ "$1" = "stop" ]; then
    shift
    cmd_stop "$@"
    exit 0
fi

if [ $# -lt 2 ]; then
    usage
fi

ADDR_PORT=$1
MOUNT_PATH=$2
EXPORT_NAME=${3:-fs}
shift 3 || shift $#

LOCK_DIR=$(dirname "$MOUNT_PATH")
LOCK_FILE="$LOCK_DIR/.efs-$(basename "$MOUNT_PATH").lock"
mkdir -p "$LOCK_DIR"
exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    echo "ERROR: another efs-fuse already holds $MOUNT_PATH ($LOCK_FILE)" >&2
    exit 1
fi

# A live healthy mount must not be stolen. Stale ENOTCONN mounts are cleared.
if is_listed_mount "$MOUNT_PATH" || mountpoint -q "$MOUNT_PATH" 2>/dev/null; then
    if pgrep -f "[e]fs-fuse .* ${MOUNT_PATH}( |$)" >/dev/null 2>&1; then
        echo "ERROR: $MOUNT_PATH is already mounted by a live efs-fuse" >&2
        exit 1
    fi
    echo "Mount point stale; unmounting first..."
    cmd_stop "$MOUNT_PATH"
fi

mkdir -p "$MOUNT_PATH"

# Larger FUSE worker stacks: write path used to put ~320 KiB frames on-stack.
if [ -z "${FUSE_THREAD_STACK:-}" ]; then
    export FUSE_THREAD_STACK=8388608
fi

# Cap glibc malloc arenas: with 200+ FUSE/RDMA threads the default
# 8-per-core arena policy fanned residual malloc churn into ~300 x 64 MiB
# arenas (16.9 GB of RSS that never returns to the OS).
if [ -z "${MALLOC_ARENA_MAX:-}" ]; then
    export MALLOC_ARENA_MAX=4
fi

LOG_DIR=$(dirname "$MOUNT_PATH")
LOG_FILE="${EFS_FUSE_LOG:-$LOG_DIR/efs-fuse-$(basename "$MOUNT_PATH").log}"

# libfuse daemonizes unless -f is set. After daemonize the parent exits 0 and
# the child's stderr is detached from this tee — writeback EIO lines vanish.
# Always force foreground so the log captures put_fragments / writeback errors.
HAS_FOREGROUND=0
for a in "$@"; do
    if [ "$a" = "-f" ] || [ "$a" = "--foreground" ]; then
        HAS_FOREGROUND=1
        break
    fi
done

echo "Stop with: $0 stop $MOUNT_PATH"
echo "Logging efs-fuse to $LOG_FILE (foreground; Ctrl-C or stop to unmount)"
echo "Write/fsync EIO details (efs_rc, ino, offset) go to this log."
# Keep a copy of stdout/stderr so the next silent death is not silent.
if [ "$HAS_FOREGROUND" = 1 ]; then
    ./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" 2>&1 | tee -a "$LOG_FILE"
else
    ./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" -f "$@" 2>&1 | tee -a "$LOG_FILE"
fi
rc=${PIPESTATUS[0]}
echo "efs-fuse exited rc=$rc" | tee -a "$LOG_FILE"
exit "$rc"
