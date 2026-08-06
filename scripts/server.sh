#!/bin/bash
set -e

if [ $# -lt 2 ]; then
    echo "Usage: $0 <addr:port> <path:quota> [join-addr:port] [extra-efsd-args...]"
    echo "  addr:port      address and port this server will bind to (must be a real local IP, not a network address)"
    echo "  path:quota     storage path, optionally followed by a quota (e.g. /data/efs/s1:5T)"
    echo "  join-addr:port optional bootstrap server to join an existing cluster"
    echo "  --no-persist   start fresh and ignore any previously persisted cluster_nodes.bin"
    exit 1
fi

# Run from the project root so efsd is found.
cd "$(dirname "$0")/.."

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

STORAGE=${PATH_QUOTA%:*}
QUOTA=${PATH_QUOTA#*:}

mkdir -p "$STORAGE"
mkdir -p "$STORAGE/log"

kill_from_pidfile() {
    local file=$1
    if [ -f "$file" ]; then
        local old_pid
        old_pid=$(cat "$file")
        if kill -0 "$old_pid" 2>/dev/null; then
            echo "Killing existing efsd process $old_pid from $file"
            # SIGTERM first, then SIGKILL if it does not exit promptly.
            kill "$old_pid" 2>/dev/null || true
            for _ in $(seq 1 30); do
                if ! kill -0 "$old_pid" 2>/dev/null; then
                    break
                fi
                sleep 0.1
            done
            if kill -0 "$old_pid" 2>/dev/null; then
                kill -9 "$old_pid" 2>/dev/null || true
                sleep 0.2
            fi
        fi
        rm -f "$file"
    fi
}

# New PID file location and legacy location at the storage root.
PID_FILE="$STORAGE/log/efsd.pid"
kill_from_pidfile "$PID_FILE"
kill_from_pidfile "$STORAGE/efsd.pid"

# Fallback: if the port is still held by an efsd process not tracked by a PID
# file (e.g. an old instance started before this script existed), kill it by
# port. We prefer lsof, then fuser; if neither is available, we leave it to the
# user.
kill_port_holder() {
    local port=$1
    local pids=""
    if command -v lsof >/dev/null 2>&1; then
        pids=$(lsof -ti TCP:"$port" 2>/dev/null || true)
    elif command -v fuser >/dev/null 2>&1; then
        pids=$(fuser "$port"/tcp 2>/dev/null || true)
    fi
    if [ -n "$pids" ]; then
        for pid in $pids; do
            # Only kill efsd processes to avoid stomping on unrelated services.
            if [ -r "/proc/$pid/cmdline" ] && tr '\0' ' ' < "/proc/$pid/cmdline" | grep -q "efsd"; then
                echo "Killing efsd process $pid holding port $port"
                kill "$pid" 2>/dev/null || true
                for _ in $(seq 1 30); do
                    if ! kill -0 "$pid" 2>/dev/null; then
                        break
                    fi
                    sleep 0.1
                done
                if kill -0 "$pid" 2>/dev/null; then
                    kill -9 "$pid" 2>/dev/null || true
                    sleep 0.2
                fi
            fi
        done
    fi
}
kill_port_holder "$PORT"

# Derive a stable node id from the address and port.
if [[ "$ADDR" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    LAST_OCTET=${BASH_REMATCH[4]}
    NODE_ID=$((LAST_OCTET * 65536 + PORT))
else
    NODE_ID=$(printf '%d' "0x$(echo -n "$ADDR_PORT" | sha256sum | head -c 8)")
fi

ARGS=(./efsd --node-id "$NODE_ID" --addr "$ADDR" --port "$PORT" --storage "$STORAGE")
if [ "$QUOTA" != "$STORAGE" ]; then
    ARGS+=(--quota "$QUOTA")
fi
if [ -n "$JOIN" ]; then
    ARGS+=(--join "$JOIN")
fi
if [ ${#EXTRA_ARGS[@]} -gt 0 ]; then
    ARGS+=("${EXTRA_ARGS[@]}")
fi

"${ARGS[@]}" >> "$STORAGE/log/efsd.log" 2>&1 &
NEW_PID=$!
echo "$NEW_PID" > "$PID_FILE"

echo "efsd started (PID $NEW_PID); logs: $STORAGE/log/efsd.log"

# Give efsd a moment to bind and report any immediate failure.
sleep 1
if ! kill -0 "$NEW_PID" 2>/dev/null; then
    echo "ERROR: efsd exited immediately. Last log lines:"
    tail -n 20 "$STORAGE/log/efsd.log" 2>/dev/null || true
    exit 1
fi
