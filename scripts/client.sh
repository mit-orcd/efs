#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 <server-addr:port> <mount-path> [export-name] [extra-efs-fuse-args...]
  $0 stop <mount-path>

  start:  mount export at mount-path (default export name: fs)
  stop:   unmount mount-path (fusermount -u, then umount)
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
    if command -v fusermount >/dev/null 2>&1; then
        fusermount -u "$mnt" 2>/dev/null || fusermount -uz "$mnt" 2>/dev/null || true
    fi
    umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true

    if is_listed_mount "$mnt" || mountpoint -q "$mnt" 2>/dev/null; then
        echo "ERROR: still mounted: $mnt" >&2
        echo "Try: fusermount -uz $mnt   or   umount -l $mnt" >&2
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

# Clear a stale/dead FUSE mount before mkdir. After efs-fuse crashes, every
# stat on the path returns ENOTCONN ("Transport endpoint is not connected").
if is_listed_mount "$MOUNT_PATH" || mountpoint -q "$MOUNT_PATH" 2>/dev/null; then
    echo "Mount point busy or stale; unmounting first..."
    cmd_stop "$MOUNT_PATH"
fi
# Always poke fusermount once more — covers ENOTCONN when /proc path differs.
if command -v fusermount >/dev/null 2>&1; then
    fusermount -u "$MOUNT_PATH" 2>/dev/null || \
        fusermount -uz "$MOUNT_PATH" 2>/dev/null || true
fi
umount "$MOUNT_PATH" 2>/dev/null || umount -l "$MOUNT_PATH" 2>/dev/null || true

mkdir -p "$MOUNT_PATH"

# Larger FUSE worker stacks: write path used to put ~320 KiB frames on-stack.
if [ -z "${FUSE_THREAD_STACK:-}" ]; then
    export FUSE_THREAD_STACK=8388608
fi

LOG_DIR=$(dirname "$MOUNT_PATH")
LOG_FILE="${EFS_FUSE_LOG:-$LOG_DIR/efs-fuse-$(basename "$MOUNT_PATH").log}"

echo "Stop with: $0 stop $MOUNT_PATH"
echo "Logging efs-fuse to $LOG_FILE"
# Keep a copy of stdout/stderr so the next silent death is not silent.
./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" 2>&1 | tee -a "$LOG_FILE"
rc=${PIPESTATUS[0]}
echo "efs-fuse exited rc=$rc" | tee -a "$LOG_FILE"
exit "$rc"
