#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 [--perf] <addr:port> <path[,path...][:quota]> [join-addr:port] [extra-efsd-args...]
  $0 stop <path[:quota]|path[,path...]|addr:port>

  start:  bind addr:port; one storage path or comma-separated 1..24 paths
          (optional :quota after the path list); optional join
  --perf: record efsd (perf record -F 499 -g) until stop.
          stop then writes flat.txt, by_thread.txt, and callers.txt under
          ~/orcd/scratch/efs/perf/efsd-<port>/ (override with EFS_PERF_DIR).
  stop:   stop by first storage path (PID file) or by addr:port
  env:    EFS_TRANSPORT=auto|tcp|rdma (default auto: RDMA if IB is up, else TCP)
          rdma is strict (no TCP fallback). Optional EFS_RDMA_DEV=<ibdev>.
EOF
    exit 1
}

# Mark files start writes so stop can find the perf directory.
perf_mark_storage() {
    local storage=$1
    storage=${storage%%,*}
    echo "$storage/log/efsd.perfdir"
}

perf_mark_port() {
    local root=${EFS_PERF_DIR:-$HOME/orcd/scratch/efs/perf}
    echo "$root/.port-$1"
}

finish_perf_dir() {
    local dir=$1 ppid i
    [ -n "$dir" ] && [ -d "$dir" ] || return 0
    python3 ./scripts/server_processes.py stop-perf "$dir/perf.pid" "$dir/efsd.data" || return 1
    if [ ! -s "$dir/efsd.data" ]; then
        echo "ERROR: no perf data at $dir/efsd.data (see $dir/perf.stderr)" >&2
        return 1
    fi
    echo "Writing perf reports in $dir"
    (
        cd "$dir"
        perf report -i efsd.data --stdio --no-children --sort dso,sym --percent-limit 0.3 -g none > flat.txt
        perf report -i efsd.data --stdio --no-children --sort comm,sym --percent-limit 0.5 -g none > by_thread.txt
        perf report -i efsd.data --stdio --children --sort sym --percent-limit 2 -g caller,0.5,callee,function,percent > callers.txt
    )
    echo "perf reports: $dir/flat.txt $dir/by_thread.txt $dir/callers.txt"
}

finish_perf_mark() {
    local mark=$1 dir
    [ -f "$mark" ] || return 0
    dir=$(cat "$mark")
    rm -f "$mark"
    finish_perf_dir "$dir"
}

start_server_perf() {
    local pid=$1 port=$2 storage=$3
    local root dir
    root=${EFS_PERF_DIR:-$HOME/orcd/scratch/efs/perf}
    dir="$root/efsd-$port"
    command -v perf >/dev/null 2>&1 || { echo "ERROR: perf not on PATH" >&2; return 1; }
    mkdir -p "$dir" "$storage/log"
    rm -f "$dir/efsd.data" "$dir/flat.txt" "$dir/by_thread.txt" "$dir/callers.txt"
    # nohup keeps $! as perf. setsid's parent is not the recorder, so
    # stop would fail to flush efsd.data.
    nohup perf record -F 499 -g -p "$pid" -o "$dir/efsd.data" \
        </dev/null >"$dir/perf.stdout" 2>"$dir/perf.stderr" &
    echo $! > "$dir/perf.pid"
    python3 ./scripts/server_processes.py record-perf "$dir/perf.pid" "$dir/efsd.data" || return 1
    echo "$dir" > "$(perf_mark_storage "$storage")"
    echo "$dir" > "$(perf_mark_port "$port")"
    echo "perf record -F 499 -g -p $pid -> $dir/efsd.data"
}

# Run from the project root so efsd is found.
cd "$(dirname "$0")/.."

kill_from_pidfile() {
    local file=$1 storage=$2
    python3 ./scripts/server_processes.py stop-file "$file" "$storage"
}

kill_port_holder() {
    local port=$1
    local pids=""
    if command -v lsof >/dev/null 2>&1; then
        pids=$(lsof -ti TCP:"$port" 2>/dev/null || true)
    elif command -v fuser >/dev/null 2>&1; then
        pids=$(fuser "$port"/tcp 2>/dev/null || true)
    fi
    if [ -z "$pids" ]; then
        return 0
    fi
    for pid in $pids; do
        if [ -r "/proc/$pid/cmdline" ] && tr '\0' ' ' < "/proc/$pid/cmdline" | grep -q "efsd"; then
            python3 ./scripts/server_processes.py stop-port "$pid" "$port"
        fi
    done
}

cmd_stop() {
    if [ $# -lt 1 ]; then
        echo "Usage: $0 stop <path[:quota]|addr:port>" >&2
        exit 1
    fi
    local target=$1
    # addr:port → kill by port
    if [[ "$target" == *:* ]] && [[ "$target" != /* ]] && [[ "$target" != ./* ]]; then
        local port=${target##*:}
        if [[ "$port" =~ ^[0-9]+$ ]]; then
    kill_port_holder "$port"
    echo "Stopped efsd on port $port (if any)"
    finish_perf_mark "$(perf_mark_port "$port")" || echo "WARNING: perf reports were not written" >&2
    return 0
        fi
    fi
    local storage=${target%:*}
    # Multi-path: PID lives under the first root.
    storage=${storage%%,*}
    kill_from_pidfile "$storage/log/efsd.pid" "$storage"
    kill_from_pidfile "$storage/efsd.pid" "$storage"
    echo "Stopped efsd for storage $storage (if any)"
    finish_perf_mark "$(perf_mark_storage "$storage")" || echo "WARNING: perf reports were not written" >&2
}

if [ $# -lt 1 ]; then
    usage
fi

PERF=0
if [ "$1" = "--perf" ]; then
    PERF=1
    shift
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
PATH_QUOTA=$2
shift 2

# Optional third positional argument is the join address.
JOIN=""
EXTRA_ARGS=()
if [ $# -gt 0 ] && [[ "$1" == *:* ]]; then
    JOIN=$1
    shift
fi
for a in "$@"; do
    if [ "$a" = "--perf" ]; then
        PERF=1
    else
        EXTRA_ARGS+=("$a")
    fi
done

ADDR=${ADDR_PORT%:*}
PORT=${ADDR_PORT##*:}

# path[,path...]:quota  — quota is after the last colon only when it looks like
# a size (digits + optional T/G/M/K). Otherwise the whole string is storage.
STORAGE=$PATH_QUOTA
QUOTA=""
if [[ "$PATH_QUOTA" =~ ^(.*):([0-9]+([TtGgMmKk][Ii]?[Bb]?)?)$ ]]; then
    STORAGE=${BASH_REMATCH[1]}
    QUOTA=${BASH_REMATCH[2]}
fi

# Create every local root; PID/log live under the first path.
FIRST_STORAGE=${STORAGE%%,*}
IFS=',' read -r -a STORAGE_PATHS <<< "$STORAGE"
for p in "${STORAGE_PATHS[@]}"; do
    mkdir -p "$p" "$p/log"
done

# New PID file location and legacy location at the storage root.
PID_FILE="$FIRST_STORAGE/log/efsd.pid"
kill_from_pidfile "$PID_FILE" "$FIRST_STORAGE"
kill_from_pidfile "$FIRST_STORAGE/efsd.pid" "$FIRST_STORAGE"
kill_port_holder "$PORT"

# Derive a stable node id from the address and port.
if [[ "$ADDR" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    LAST_OCTET=${BASH_REMATCH[4]}
    NODE_ID=$((LAST_OCTET * 65536 + PORT))
else
    NODE_ID=$(printf '%d' "0x$(echo -n "$ADDR_PORT" | sha256sum | head -c 8)")
fi

ARGS=(./efsd --node-id "$NODE_ID" --addr "$ADDR" --port "$PORT" --storage "$STORAGE")
if [ -n "$QUOTA" ]; then
    ARGS+=(--quota "$QUOTA")
fi
if [ -n "$JOIN" ]; then
    ARGS+=(--join "$JOIN")
fi
if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
    ARGS+=("${EXTRA_ARGS[@]}")
fi

LOG_FILE="$FIRST_STORAGE/log/efsd.log"
# Mark where this start's log begins so we can surface join/bind errors.
LOG_MARK=$(wc -l < "$LOG_FILE" 2>/dev/null || echo 0)

"${ARGS[@]}" >> "$LOG_FILE" 2>&1 &
NEW_PID=$!
echo "$NEW_PID" > "$PID_FILE"
python3 ./scripts/server_processes.py record "$PID_FILE" "$FIRST_STORAGE"

echo "efsd started (PID $NEW_PID); logs: $LOG_FILE"
echo "Stop with: $0 stop $FIRST_STORAGE   # or: $0 stop $ADDR_PORT"

# Give efsd a moment to bind and report any immediate failure.
sleep 1
if ! kill -0 "$NEW_PID" 2>/dev/null; then
    echo "ERROR: efsd exited immediately. Last log lines:"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    exit 1
fi

if [ "$PERF" = 1 ]; then
    start_server_perf "$NEW_PID" "$PORT" "$FIRST_STORAGE"
fi

# Join failures are non-fatal (efsd runs standalone and retries), but they
# must be visible on the terminal — not only buried in the log.
if [ -n "$JOIN" ]; then
    NEW_LOG=$(tail -n +"$((LOG_MARK + 1))" "$LOG_FILE" 2>/dev/null || true)
    if echo "$NEW_LOG" | grep -qE 'Cannot connect to peer|Could not join cluster|Invalid join address|Join rejected|Join failed|No HELLO_ACK'; then
        echo "WARNING: join to $JOIN failed; efsd is running standalone and will retry in the background." >&2
        echo "$NEW_LOG" | grep -E 'Cannot connect to peer|Could not join cluster|Invalid join address|Join rejected|Join failed|No HELLO_ACK' >&2 || true
        echo "See full log: $LOG_FILE" >&2
    elif ! echo "$NEW_LOG" | grep -q 'Joined cluster'; then
        echo "WARNING: join to $JOIN was requested but no join success was logged yet; check $LOG_FILE" >&2
    fi
fi
