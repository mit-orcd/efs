#!/bin/bash
set -e

usage() {
    cat <<EOF
Usage:
  $0 <addr:port> <path[,path...][:quota]> [join-addr:port] [extra-efsd-args...]
  $0 stop <path[:quota]|path[,path...]|addr:port>

  start:  bind addr:port; one storage path or comma-separated 1..24 paths
          (optional :quota after the path list); optional join
  stop:   stop by first storage path (PID file) or by addr:port
EOF
    exit 1
}

# Run from the project root so efsd is found.
cd "$(dirname "$0")/.."

kill_pid_graceful() {
    local pid=$1
    local label=${2:-process}
    if ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    echo "Stopping $label (PID $pid)"
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 30); do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
        sleep 0.2
    fi
}

kill_from_pidfile() {
    local file=$1
    if [ -f "$file" ]; then
        local old_pid
        old_pid=$(cat "$file")
        kill_pid_graceful "$old_pid" "efsd from $file"
        rm -f "$file"
    fi
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
            kill_pid_graceful "$pid" "efsd holding port $port"
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
            return 0
        fi
    fi
    local storage=${target%:*}
    # Multi-path: PID lives under the first root.
    storage=${storage%%,*}
    kill_from_pidfile "$storage/log/efsd.pid"
    kill_from_pidfile "$storage/efsd.pid"
    echo "Stopped efsd for storage $storage (if any)"
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
PATH_QUOTA=$2
shift 2

# Optional third positional argument is the join address.
JOIN=""
EXTRA_ARGS=()
if [ $# -gt 0 ] && [[ "$1" == *:* ]]; then
    JOIN=$1
    shift
fi
EXTRA_ARGS+=("$@")

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
kill_from_pidfile "$PID_FILE"
kill_from_pidfile "$FIRST_STORAGE/efsd.pid"
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

echo "efsd started (PID $NEW_PID); logs: $LOG_FILE"
echo "Stop with: $0 stop $FIRST_STORAGE   # or: $0 stop $ADDR_PORT"

# Give efsd a moment to bind and report any immediate failure.
sleep 1
if ! kill -0 "$NEW_PID" 2>/dev/null; then
    echo "ERROR: efsd exited immediately. Last log lines:"
    tail -n 20 "$LOG_FILE" 2>/dev/null || true
    exit 1
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
