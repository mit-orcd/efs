#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 [--perf] [--strace] <server-addr:port> <mount-path> [extra-efs-fuse-args...]
  $0 stop [--force-discard] <mount-path>

  start:  mount at mount-path.
          Returns only after the mount is serving (stat works).
          Pass -f to stay in the foreground.
  --perf: record efs-fuse (perf record -F 499 -g) until stop.
          stop then writes flat.txt, by_thread.txt, and callers.txt under
          ~/orcd/scratch/efs/perf/<mount-name>/ (override with EFS_PERF_DIR).
  --strace: strace -f -tt -T the daemon until stop, into the same dir as
          fuse.strace. stop writes strace-summary.txt (per-syscall count /
          total / max) and strace-long.txt (work syscalls >= 100 ms, waits
          >= 2 s). EFS_STRACE_EXPR narrows it (passed as -e, e.g.
          'trace=fsync,writev,recvfrom,futex'); a full trace of a busy
          client is ~1 GB per 10 min. Needs kernel.yama.ptrace_scope=0.
          With --perf too, the perf shares include the ptrace stops.
  stop:   quiesce and drain while mounted; refuse if unresolved after 60 s.
          --force-discard explicitly permits loss and lazy detach.
  env:    EFS_TRANSPORT=auto|tcp|rdma (default auto: RDMA if IB is up, else TCP)
          rdma is strict (no TCP fallback). Optional EFS_RDMA_DEV=<ibdev>.
EOF
    exit 1
}

# Directory start wrote so stop can find the perf files for this mount.
perf_mark() {
    echo "$(dirname "$1")/.efs-$(basename "$1").perfdir"
}

# Recorder dir for this mount (EFS_PERF_DIR/<mount-name>), shared by --perf
# and --strace.
trace_dir_for() {
    local root=${EFS_PERF_DIR:-$HOME/orcd/scratch/efs/perf}
    echo "$root/$(basename "$1")"
}

# SIGINT a recorder whose pid is in $1, wait up to 15 s, then SIGTERM.
stop_recorder() {
    local pidfile=$1 ppid i
    ppid=$(cat "$pidfile" 2>/dev/null || true)
    [ -n "$ppid" ] || return 0
    if kill -0 "$ppid" 2>/dev/null; then
        kill -INT "$ppid" 2>/dev/null || true
        for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            kill -0 "$ppid" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$ppid" 2>/dev/null; then
            kill -TERM "$ppid" 2>/dev/null || true
        fi
    fi
    rm -f "$pidfile"
}

# Stop whichever recorders start attached and write their reports.
finish_recorders() {
    local mnt=$1 mark dir rc=0
    mark=$(perf_mark "$mnt")
    [ -f "$mark" ] || return 0
    dir=$(cat "$mark")
    rm -f "$mark"
    if [ -f "$dir/perf.pid" ] || [ -s "$dir/fuse.data" ]; then
        finish_perf "$dir" || rc=1
    fi
    if [ -f "$dir/strace.pid" ] || [ -s "$dir/fuse.strace" ]; then
        finish_strace "$dir" || rc=1
    fi
    return $rc
}

# strace -f -tt -T output: "<tid> <HH:MM:SS.us> name(args) = ret <dur>", or
# "<tid> <ts> <... name resumed>) = ret <dur>" for an interleaved call.
finish_strace() {
    local dir=$1
    stop_recorder "$dir/strace.pid"
    if [ ! -s "$dir/fuse.strace" ]; then
        echo "ERROR: no strace data at $dir/fuse.strace (see $dir/strace.stderr)" >&2
        return 1
    fi
    echo "Writing strace summary in $dir"
    awk -v longf="$dir/strace-long.txt" '
    function iswait(n) {
        return (n == "futex" || n == "poll" || n == "ppoll" || n == "epoll_wait" || n == "read" ||
                n == "recvfrom" || n == "recvmsg" || n == "accept" || n == "accept4" || n == "select" ||
                n == "nanosleep" || n == "clock_nanosleep" || n == "wait4" || n == "restart_syscall" ||
                n == "pselect6");
    }
    BEGIN { printf "" > longf }
    {
        if (!match($0, /<[0-9]+\.[0-9]+>$/)) next;
        dur = substr($0, RSTART + 1, RLENGTH - 2) + 0;
        if ($3 ~ /^<\.\.\./) name = $4; else { name = $3; sub(/\(.*/, "", name); }
        cnt[name]++; tot[name] += dur; if (dur > mx[name]) mx[name] = dur;
        if ((!iswait(name) && dur >= 0.1) ||
            (iswait(name) && dur >= 2.0 && name != "read" && name != "futex" && name != "accept" && name != "accept4")) {
            line = $0; if (length(line) > 160) line = substr(line, 1, 160);
            printf "%s %s %-14s %8.3f %s\n", $2, $1, name, dur, (iswait(name) ? "WAIT" : "WORK") >> longf;
            if (!iswait(name)) printf "    %s\n", line >> longf;
        }
        if (first == "") first = $2; last = $2;
    }
    END {
        printf "window %s .. %s\n", first, last;
        printf "%-20s %10s %12s %10s\n", "syscall", "count", "total_s", "max_s";
        for (n in cnt) printf "%-20s %10d %12.3f %10.3f\n", n, cnt[n], tot[n], mx[n] | "sort -k3 -rn";
    }' "$dir/fuse.strace" > "$dir/strace-summary.txt"
    echo "strace reports: $dir/strace-summary.txt $dir/strace-long.txt (raw: $dir/fuse.strace)"
}

# Wait for perf record to flush fuse.data, then the three reports.
finish_perf() {
    local dir=$1
    stop_recorder "$dir/perf.pid"
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

# The daemon pid from the log; the parent has already exited, so a wrapper
# around the client.sh command would record nothing.
daemon_pid_from_log() {
    local log=$1 pid
    pid=$(grep -o 'efs-fuse daemon pid=[0-9]*' "$log" | tail -1 | cut -d= -f2)
    if [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: could not find a live efs-fuse pid in $log" >&2
        return 1
    fi
    echo "$pid"
}

# Attach perf to the running efs-fuse.
start_perf() {
    local mnt=$1 log=$2
    local dir pid
    dir=$(trace_dir_for "$mnt")
    command -v perf >/dev/null 2>&1 || { echo "ERROR: perf not on PATH" >&2; return 1; }
    mkdir -p "$dir"
    pid=$(daemon_pid_from_log "$log") || return 1
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

# Attach strace to the running efs-fuse (all threads, timestamps, durations).
start_strace() {
    local mnt=$1 log=$2
    local dir pid
    dir=$(trace_dir_for "$mnt")
    command -v strace >/dev/null 2>&1 || { echo "ERROR: strace not on PATH" >&2; return 1; }
    mkdir -p "$dir"
    pid=$(daemon_pid_from_log "$log") || return 1
    rm -f "$dir/fuse.strace" "$dir/strace-summary.txt" "$dir/strace-long.txt"
    local -a expr=()
    if [ -n "${EFS_STRACE_EXPR:-}" ]; then
        expr=(-e "$EFS_STRACE_EXPR")
    fi
    nohup strace -f -tt -T -qq "${expr[@]}" -o "$dir/fuse.strace" -p "$pid" \
        </dev/null >"$dir/strace.stdout" 2>"$dir/strace.stderr" &
    echo $! > "$dir/strace.pid"
    echo "$dir" > "$(perf_mark "$mnt")"
    sleep 1
    if ! kill -0 "$(cat "$dir/strace.pid")" 2>/dev/null; then
        echo "ERROR: strace exited at once (ptrace_scope? see $dir/strace.stderr)" >&2
        cat "$dir/strace.stderr" >&2 || true
        rm -f "$dir/strace.pid"
        return 1
    fi
    echo "strace -f -tt -T ${expr[*]} -p $pid -> $dir/fuse.strace"
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
    local force=0
    if [ "${1:-}" = "--force-discard" ]; then
        force=1
        shift
    fi
    if [ $# -ne 1 ]; then
        echo "Usage: $0 stop [--force-discard] <mount-path>" >&2
        exit 1
    fi
    local mnt=$1

    # Do not trust [ -e ] / [ -d ]: a dead FUSE mount returns ENOTCONN and
    # those tests look like "does not exist" even though /proc/mounts lists it.
    local listed=0
    if is_listed_mount "$mnt" || mountpoint -q "$mnt" 2>/dev/null; then
        listed=1
    fi

    local process_rc=0
    python3 ./scripts/client_processes.py has "$mnt" || process_rc=$?
    if [ "$process_rc" -gt 1 ]; then
        echo "ERROR: cannot discover client processes; stop refused" >&2
        return 1
    fi
    if [ "$listed" -eq 1 ] || [ "$process_rc" -eq 0 ]; then
        local control_rc=0
        if [ "$force" -eq 1 ]; then
            ./efs-fuse --stop "$mnt" --force-discard || control_rc=$?
        else
            ./efs-fuse --stop "$mnt" || control_rc=$?
        fi
        if [ "$control_rc" -ne 0 ]; then
            if [ "$force" -eq 1 ] && [ "$control_rc" -eq 3 ]; then
                echo "WARNING: explicit discard of an old/dead client; per-record diagnostics unavailable" >&2
            else
                ./efs-fuse --resume "$mnt" || true
                echo "Stop refused; mount and daemon retained. Resolve pending writes or use --force-discard explicitly." >&2
                return 1
            fi
        fi
    fi
    echo "Unmounting $mnt"
    if [ "$force" -eq 1 ]; then
        fusermount3 -u "$mnt" 2>/dev/null || fusermount3 -uz "$mnt" 2>/dev/null || true
        umount "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true
    else
        fusermount3 -u "$mnt" 2>/dev/null || umount "$mnt" 2>/dev/null || true
    fi

    if is_listed_mount "$mnt" || mountpoint -q "$mnt" 2>/dev/null; then
        ./efs-fuse --resume "$mnt" || true
        echo "ERROR: still mounted: $mnt" >&2
        echo "Stop did not detach; mutation admission resumed. Use stop --force-discard only to accept loss." >&2
        exit 1
    fi

    if [ "$process_rc" -eq 0 ]; then
        python3 ./scripts/client_processes.py retire "$mnt" || return 1
    fi
    if [ "$listed" -eq 1 ]; then
        echo "Unmounted $mnt"
    else
        echo "Not mounted: $mnt (cleared any stale FUSE handle)"
    fi
    finish_recorders "$mnt" || echo "WARNING: some recorder reports were not written" >&2
}

if [ $# -lt 1 ]; then
    usage
fi

if [ "$1" = "stop" ]; then
    shift
    cmd_stop "$@"
    exit $?
fi

PERF=0
STRACE=0
while [ $# -gt 0 ]; do
    case "$1" in
    --perf) PERF=1; shift ;;
    --strace) STRACE=1; shift ;;
    *) break ;;
    esac
done

if [ $# -lt 2 ]; then
    usage
fi

ADDR_PORT=$1
MOUNT_PATH=$2
shift 2
# A leftover export name (the old third argument) is not a fuse option.
# Drop one such word. Options and their arguments (-o allow_other) stay.
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    shift
fi
ARGS=()
for a in "$@"; do
    if [ "$a" = "--perf" ]; then
        PERF=1
    elif [ "$a" = "--strace" ]; then
        STRACE=1
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
    if [ "$PERF" = 1 ] || [ "$STRACE" = 1 ]; then
        # No daemon fork in -f, so the recorders wrap the command:
        # perf record -- strace -f -- efs-fuse (perf follows children).
        dir=$(trace_dir_for "$MOUNT_PATH")
        mkdir -p "$dir"
        rm -f "$dir/fuse.data" "$dir/flat.txt" "$dir/by_thread.txt" "$dir/callers.txt" "$dir/perf.pid" \
              "$dir/fuse.strace" "$dir/strace-summary.txt" "$dir/strace-long.txt" "$dir/strace.pid"
        echo "$dir" > "$(perf_mark "$MOUNT_PATH")"
        wrap=()
        if [ "$PERF" = 1 ]; then
            wrap+=(perf record -F 499 -g -o "$dir/fuse.data" --)
            echo "perf record -F 499 -g -> $dir/fuse.data"
        fi
        if [ "$STRACE" = 1 ]; then
            wrap+=(strace -f -tt -T -qq)
            [ -n "${EFS_STRACE_EXPR:-}" ] && wrap+=(-e "$EFS_STRACE_EXPR")
            wrap+=(-o "$dir/fuse.strace" --)
            echo "strace -f -tt -T -> $dir/fuse.strace"
        fi
        set +e
        "${wrap[@]}" ./efs-fuse "$ADDR_PORT" "$EXPORT_NAME" "$MOUNT_PATH" "$@" \
            >>"$LOG_FILE" 2>&1
        rc=$?
        set -e
        echo "efs-fuse exited rc=$rc" | tee -a "$LOG_FILE"
        finish_recorders "$MOUNT_PATH" || echo "WARNING: some recorder reports were not written" >&2
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
rc=0
if [ "$PERF" = 1 ]; then
    start_perf "$MOUNT_PATH" "$LOG_FILE" || rc=1
fi
if [ "$STRACE" = 1 ]; then
    start_strace "$MOUNT_PATH" "$LOG_FILE" || rc=1
fi
exit "$rc"
