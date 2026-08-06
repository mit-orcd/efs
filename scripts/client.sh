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

cmd_stop() {
    if [ $# -lt 1 ]; then
        echo "Usage: $0 stop <mount-path>" >&2
        exit 1
    fi
    local mnt=$1

    if [ ! -e "$mnt" ] && [ ! -L "$mnt" ] && [ ! -d "$mnt" ]; then
        echo "Mount path does not exist: $mnt"
        exit 1
    fi

    if mountpoint -q "$mnt" 2>/dev/null; then
        echo "Unmounting $mnt"
        if command -v fusermount >/dev/null 2>&1; then
            fusermount -u "$mnt" 2>/dev/null || fusermount -uz "$mnt" 2>/dev/null || true
        fi
        if mountpoint -q "$mnt" 2>/dev/null; then
            umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true
        fi
        if mountpoint -q "$mnt" 2>/dev/null; then
            echo "ERROR: still mounted: $mnt" >&2
            exit 1
        fi
        echo "Unmounted $mnt"
    else
        echo "Not mounted: $mnt"
        # Best-effort cleanup of a stuck FUSE handle.
        if command -v fusermount >/dev/null 2>&1; then
            fusermount -u "$mnt" 2>/dev/null || fusermount -uz "$mnt" 2>/dev/null || true
        fi
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

mkdir -p "$MOUNT_PATH"

# Clear a stale mount before starting.
if mountpoint -q "$MOUNT_PATH" 2>/dev/null; then
    echo "Mount point busy; unmounting first..."
    cmd_stop "$MOUNT_PATH"
fi

echo "Stop with: $0 stop $MOUNT_PATH"
./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@"
