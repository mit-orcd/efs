#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 [--perf] <server-addr:port> <mount-path> [extra-efs-fuse-args...]
  $0 stop <mount-path>

  start:  mount at mount-path.
          Returns only after the mount is serving (stat works).
          Pass -f to stay in the foreground.
  --perf: record efs-fuse (perf record -F 499 -g) until stop.
          stop then writes flat.txt, by_thread.txt, and callers.txt under
          ~/orcd/scratch/efs/perf/<mount-name>/ (override with EFS_PERF_DIR).
  stop:   unmount mount-path (fusermount3 -u, then umount)
  env:    EFS_TRANSPORT=auto|tcp|rdma (default auto: RDMA if IB is up, else TCP)
          rdma is strict (no TCP fallback). Optional EFS_RDMA_DEV=<ibdev>.
EOF
    exit 1
}

# Directory start wrote so stop can find the perf files for this mount.
perf_mark() {
    echo "$(dirname "$1")/.efs-$(basename "$1").perfdir"
}

# SIGINT perf record so it flushes fuse.data, then the three reports.
finish_perf() {
    local mnt=$1 mark dir ppid i
    mark=$(perf_mark "$mnt")
    [ -f "$mark" ] || return 0
    dir=$(cat "$mark")
    ppid=$(cat "$dir/perf.pid" 2>/dev/null || true)
    if [ -n "$ppid" ] && kill -0 "$ppid" 2>/dev/null; then
        kill -INT "$ppid" 2>/dev/null || true
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            kill -0 "$ppid" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$ppid" 2>/dev/null; then
            kill -TERM "$ppid" 2>/dev/null || true
        fi
    fi
    rm -f "$mark"
    if [ ! -s "$dir/fuse.data" ]; then
        echo "ERROR: no perf data at $dir/fuse.data (see $dir/fuse.stderr)" >&2
        return 1
    fi
    echo "Writing perf reports in $dir"
    (
        cd "$dir"
        perf report -i fuse.data --stdio --no-children --sort dso,sym --percent-limit 0.3 -g none > flat.txt
        perf report -i fuse.data --stdio --no-children --sort comm,sym --percent-limit 0.5 -g none > by_thread.txt
        perf report -i fuse.data --stdio --children --sort sym --percent-limit 2 -g caller,0.5,callee,function,percent > callers.txt
    )
    echo "perf reports: $dir/flat.txt $dir/by_thread.txt $dir/callers.txt"
}

# Attach perf to a running efs-fuse. The daemon's parent has already exited,
# so wrapping the client.sh command would record nothing.
start_perf() {
    local mnt=$1 log=$2
    local root dir pid
    root=${EFS_PERF_DIR:-$HOME/orcd/scratch/efs/perf}
    dir="$root/$(basename "$mnt")"
    command -v perf >/dev/null 2>&1 || { echo "ERROR: perf not on PATH" >&2; return 1; }
    mkdir -p "$dir"
    pid=$(grep -o 'efs-fuse daemon pid=[0-9]*' "$log" | tail -1 | cut -d= -f2)
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: --perf could not find a live efs-fuse pid in $log" >&2
        return 1
    fi
    rm -f "$dir/fuse.data" "$dir/flat.txt" "$dir/by_thread.txt" "$dir/callers.txt"
    # nohup keeps $! as perf. setsid forks when it is a process-group
    # leader and the parent only waits; signalling that parent leaves
    # perf running and fuse.data unflushed.
    nohup perf record -F 499 -g -p "$pid" -o "$dir/fuse.data" \
        </dev/null >"$dir/perf.stdout" 2>"$dir/fuse.stderr" &
    echo $! > "$dir/perf.pid"
    echo "$dir" > "$(perf_mark "$mnt")"
    echo "perf record -F 499 -g -p $pid -> $dir/fuse.data"
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
    finish_perf "$mnt" || echo "WARNING: perf reports were not written" >&2
}

if [ $# -lt 1 ]; then
    usage
fi

if [ "$1" = "stop" ]; then
    shift
    cmd_stop "$@"
    exit 0
fi

PERF=0
if [ "$1" = "--perf" ]; then
    PERF=1
    shift
fi

if [ $# -lt 2 ]; then
    usage
fi

ADDR_PORT=$1
MOUNT_PATH=$2
shift 2
# A leftover export name (the old third argument) is not a fuse option.
# Drop one such word. Options and their arguments (-o allow_other) stay.
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ] && [ "$1" != "--perf" ]; then
    shift
fi
ARGS=()
for a in "$@"; do
    if [ "$a" = "--perf" ]; then
        PERF=1
    else
        ARGS+=("$a")
    fi
done
set -- "${ARGS[@]}"
# efs-fuse still requires a name token. The server ignores it (one export).
EXPORT_NAME=fs

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

# Default: efs-fuse daemonizes and its parent exits 0 only after FUSE_INIT
# (the kernel mount is actually serving). Pass -f / --foreground to stay
# attached and log through tee (valgrind, interactive debug).
HAS_FOREGROUND=0
for a in "$@"; do
    if [ "$a" = "-f" ] || [ "$a" = "--foreground" ]; then
        HAS_FOREGROUND=1
        break
    fi
done

echo "Stop with: $0 stop $MOUNT_PATH"
echo "Logging efs-fuse to $LOG_FILE"

if [ "$HAS_FOREGROUND" = 1 ]; then
    echo "Foreground; Ctrl-C or stop to unmount"
    if [ "$PERF" = 1 ]; then
        root=${EFS_PERF_DIR:-$HOME/orcd/scratch/efs/perf}
        dir="$root/$(basename "$MOUNT_PATH")"
        mkdir -p "$dir"
        rm -f "$dir/fuse.data" "$dir/flat.txt" "$dir/by_thread.txt" "$dir/callers.txt" "$dir/perf.pid"
        echo "$dir" > "$(perf_mark "$MOUNT_PATH")"
        echo "perf record -F 499 -g -> $dir/fuse.data"
        set +e
        perf record -F 499 -g -o "$dir/fuse.data" -- \
            ./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" \
            >>"$LOG_FILE" 2>&1
        rc=$?
        set -e
        echo "efs-fuse exited rc=$rc" | tee -a "$LOG_FILE"
        finish_perf "$MOUNT_PATH" || echo "WARNING: perf reports were not written" >&2
        exit "$rc"
    fi
    ./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" 2>&1 | tee -a "$LOG_FILE"
    rc=${PIPESTATUS[0]}
    echo "efs-fuse exited rc=$rc" | tee -a "$LOG_FILE"
    exit "$rc"
fi

set +e
./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" >>"$LOG_FILE" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
    echo "ERROR: efs-fuse failed rc=$rc (see $LOG_FILE)" >&2
    tail -n 20 "$LOG_FILE" >&2 || true
    exit "$rc"
fi
# Parent returned 0 after FUSE_INIT. Confirm a getattr actually works —
# /proc/mounts can list the path a moment before the loop is reading.
if ! timeout 3 stat "$MOUNT_PATH" >/dev/null 2>&1; then
    echo "ERROR: $MOUNT_PATH is not serving (see $LOG_FILE)" >&2
    tail -n 20 "$LOG_FILE" >&2 || true
    exit 1
fi
echo "Mounted $MOUNT_PATH"
if [ "$PERF" = 1 ]; then
    start_perf "$MOUNT_PATH" "$LOG_FILE"
fi
exit 0
