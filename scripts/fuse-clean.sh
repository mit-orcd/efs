#!/bin/bash
# Unmount efs-fuse mounts on this node without needing the mount path.
# Only touches fuse.efs-fuse / efs-fuse — never sshfs or other FUSE.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $0 [--kill] [--dry-run]

  Finds every efs-fuse mount on this host (from /proc/mounts / findmnt)
  and unmounts it (lazy if busy or ENOTCONN-stale).

  --kill     also kill leftover efs-fuse client processes (never efsd)
  --dry-run  list what would be unmounted / killed, change nothing
EOF
    exit 1
}

KILL=0
DRY=0
for arg in "$@"; do
    case "$arg" in
        --kill)    KILL=1 ;;
        --dry-run) DRY=1 ;;
        -h|--help) usage ;;
        *)         echo "Unknown option: $arg" >&2; usage ;;
    esac
done

is_efs_fuse_mount() {
    local src=$1 fstype=$2
    case "$fstype" in
        fuse.efs-fuse) return 0 ;;
        fuse)
            case "$src" in
                efs-fuse|*/efs-fuse) return 0 ;;
            esac
            ;;
    esac
    return 1
}

still_mounted() {
    local mnt=$1
    if command -v findmnt >/dev/null 2>&1; then
        findmnt -n --target "$mnt" >/dev/null 2>&1
        return $?
    fi
    awk -v want="$mnt" '
        {
            m = $2
            gsub(/\\040/, " ", m)
            gsub(/\\011/, "\t", m)
            if (m == want) exit 0
        }
        END { exit 1 }
    ' /proc/mounts
}

# Discover efs-fuse mount points (live or ENOTCONN-stale).
list_efs_fuse_mounts() {
    if command -v findmnt >/dev/null 2>&1; then
        findmnt -n -t fuse.efs-fuse -o TARGET 2>/dev/null || true
        # Some entries show fstype "fuse" with SOURCE "efs-fuse"
        findmnt -n -t fuse -o TARGET,SOURCE 2>/dev/null \
            | awk '$2 == "efs-fuse" || $2 ~ /\/efs-fuse$/ { print $1 }' || true
        return
    fi

    while read -r src mnt fstype rest; do
        [ -n "${mnt:-}" ] || continue
        mnt=$(printf '%b' "$mnt")
        if is_efs_fuse_mount "$src" "$fstype"; then
            echo "$mnt"
        fi
    done < /proc/mounts
}

unmount_one() {
    local mnt=$1
    echo "Unmounting efs-fuse at $mnt"
    if [ "$DRY" -eq 1 ]; then
        return 0
    fi
    if command -v fusermount >/dev/null 2>&1; then
        fusermount -u "$mnt" 2>/dev/null || fusermount -uz "$mnt" 2>/dev/null || true
    fi
    if still_mounted "$mnt"; then
        umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true
    fi
    if still_mounted "$mnt"; then
        echo "ERROR: still mounted: $mnt" >&2
        return 1
    fi
    echo "  OK"
}

mapfile -t MOUNTS < <(list_efs_fuse_mounts | awk 'NF' | sort -u)

if [ ${#MOUNTS[@]} -eq 0 ]; then
    echo "No efs-fuse mounts on $(hostname -s)"
else
    echo "Found ${#MOUNTS[@]} efs-fuse mount(s) on $(hostname -s):"
    rc=0
    for mnt in "${MOUNTS[@]}"; do
        unmount_one "$mnt" || rc=1
    done
    if [ "$rc" -ne 0 ]; then
        echo "Some mounts could not be removed (try as root, or fuser -vm <path>)." >&2
        exit "$rc"
    fi
fi

if [ "$KILL" -eq 1 ]; then
    # Only efs-fuse clients — never efsd.
    PIDS=$(pgrep -f '/efs-fuse |/efs-fuse$' 2>/dev/null || true)
    if [ -z "$PIDS" ]; then
        echo "No efs-fuse processes"
    else
        echo "efs-fuse PIDs: $PIDS"
        if [ "$DRY" -eq 0 ]; then
            # shellcheck disable=SC2086
            kill $PIDS 2>/dev/null || true
            sleep 0.3
            # shellcheck disable=SC2086
            kill -9 $PIDS 2>/dev/null || true
            echo "Killed efs-fuse processes"
        fi
    fi
fi

echo "Done."
