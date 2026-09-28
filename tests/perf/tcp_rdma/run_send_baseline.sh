#!/bin/bash
# RDMA SEND hardware baseline via perftest (ib_send_lat / ib_send_bw).
# This is not efs, not TCP, and not a durable filesystem completion.
# It does not talk to efsd. Port 19810 is refused.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PORT=${EFS_BASELINE_PORT:-19961}
HOST=${EFS_BASELINE_HOST:-127.0.0.1}
REPEATS=${EFS_BASELINE_REPEATS:-5}
DEV=${EFS_IB_DEV:-}
SERVER_ALREADY=${EFS_BASELINE_SERVER_ALREADY:-0}

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTDIR" >&2
    exit 2
fi
OUT=$1
mkdir -p "$OUT"
case "$PORT" in
    19810|19820)
        echo "refusing port $PORT" >&2
        exit 2
        ;;
esac

# Same rule as efs: an active InfiniBand port, not the first mlx5
# directory entry. On these hosts mlx5_0 is 25 Gb Ethernet and mlx5_2
# is the 200 Gb HCA. EFS_IB_DEV still overrides.
if [[ -z "$DEV" && -d /sys/class/infiniband ]]; then
    for d in /sys/class/infiniband/*; do
        layer=$(cat "$d/ports/1/link_layer" 2>/dev/null || true)
        state=$(cat "$d/ports/1/state" 2>/dev/null || true)
        if [[ "$layer" == InfiniBand && "$state" == 4:* ]]; then
            DEV=$(basename "$d")
            break
        fi
    done
fi

if ! command -v ib_send_lat >/dev/null 2>&1 || ! command -v ib_send_bw >/dev/null 2>&1; then
    python3 - "$OUT" <<'PY'
import json, sys
out = sys.argv[1]
row = {
    "role": "send_baseline",
    "status": "tool_missing",
    "completion": "ibv_send",
    "note": "ib_send_lat and ib_send_bw were not on PATH. No number was invented.",
}
open(out + "/results.jsonl", "w").write(json.dumps(row) + "\n")
open(out + "/config.json", "w").write(json.dumps({
    "bench": "ib_send_lat/ib_send_bw",
    "completion": "ibv_send",
    "status": "tool_missing",
    "live_cluster": "not used",
}, indent=2) + "\n")
print("perftest is not installed; wrote", out + "/results.jsonl")
PY
    exit 2
fi
if [[ -z "$DEV" ]]; then
    echo "no HCA: set EFS_IB_DEV" >&2
    exit 2
fi

help_lat=$(ib_send_lat --help 2>&1 || true)
WARM=()
if grep -q perform_warm_up <<<"$help_lat"; then
    WARM=(--perform_warm_up)
fi

export OUT PORT HOST REPEATS DEV
python3 - <<'PY'
import datetime, json, os, socket
cfg = {
    "bench": "ib_send_lat/ib_send_bw",
    "completion": "ibv_send",
    "not_efs": True,
    "not_filesystem_durability": True,
    "live_cluster": "not used; ports 19810 and 19820 are refused",
    "device": os.environ["DEV"],
    "host": os.environ["HOST"],
    "port": int(os.environ["PORT"]),
    "repeats": int(os.environ["REPEATS"]),
    "placement": "loopback" if os.environ["HOST"] in ("127.0.0.1", "localhost")
                 else os.environ["HOST"],
    "hostname": socket.gethostname(),
    "started_utc": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
    "note": "RC SEND/RECV, the same verb efs uses. Not RDMA WRITE. "
            "Quote a two-host run; loopback is a wiring check.",
}
open(os.path.join(os.environ["OUT"], "config.json"), "w").write(
    json.dumps(cfg, indent=2) + "\n")
PY

: >"$OUT/results.jsonl"
SRV=
cleanup() {
    if [[ -n "${SRV:-}" ]]; then
        kill "$SRV" 2>/dev/null || true
        wait "$SRV" 2>/dev/null || true
        SRV=
    fi
}
trap cleanup EXIT

run_one() {
    local tool=$1 size=$2 depth=$3 rep=$4
    local tag="${tool}_s${size}_d${depth}_r${rep}"
    local slog=$OUT/$tag.server
    local clog=$OUT/$tag.client
    local extra=()
    if [[ "$tool" == ib_send_bw ]]; then
        extra+=(-t "$depth")
    fi
    if [[ "$SERVER_ALREADY" != 1 ]]; then
        # perftest fully buffers stdout when it is not a tty, so the
        # "Waiting for client" line never hits the log and the 5 s poll
        # kills a healthy server. Line-buffer it.
        # This perftest refuses a handshake unless both sides pass
        # the same -s and -n (server defaults are 2 bytes and 1000
        # iterations).
        if command -v stdbuf >/dev/null 2>&1; then
            stdbuf -oL -eL "$tool" -d "$DEV" -p "$PORT" -s "$size" -n 5000 \
                "${WARM[@]}" "${extra[@]}" >"$slog" 2>&1 &
        else
            "$tool" -d "$DEV" -p "$PORT" -s "$size" -n 5000 \
                "${WARM[@]}" "${extra[@]}" >"$slog" 2>&1 &
        fi
        SRV=$!
        local i ready=0
        for i in $(seq 1 100); do
            if grep -q -e "Waiting" -e "waiting" -e "local address" "$slog" 2>/dev/null; then
                ready=1
                break
            fi
            if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then
                ready=1
                break
            fi
            if ! kill -0 "$SRV" 2>/dev/null; then
                break
            fi
            sleep 0.05
        done
        if [[ $ready -ne 1 ]]; then
            echo "baseline server did not become ready ($tag)" >&2
            cleanup
            return 1
        fi
    fi
    set +e
    if command -v stdbuf >/dev/null 2>&1; then
        stdbuf -oL -eL "$tool" -d "$DEV" -p "$PORT" -s "$size" -n 5000 \
            "${WARM[@]}" "${extra[@]}" "$HOST" >"$clog" 2>&1
    else
        "$tool" -d "$DEV" -p "$PORT" -s "$size" -n 5000 "${WARM[@]}" \
            "${extra[@]}" "$HOST" >"$clog" 2>&1
    fi
    local rc=$?
    set -e
    cleanup
    OUT="$OUT" TAG="$tag" TOOL="$tool" SIZE="$size" DEPTH="$depth" REP="$rep" \
        RC="$rc" python3 - <<'PY'
import json, os
out = os.environ["OUT"]
path = os.path.join(out, os.environ["TAG"] + ".client")
text = open(path, encoding="utf-8", errors="replace").read()
# Column titles contain spaces ("BW peak[MiB/sec]"), so split()
# cannot be zipped against the header. Both tools print a fixed
# field order on the numeric line.
parsed = None
for line in text.splitlines():
    s = line.strip()
    if not s[:1].isdigit():
        continue
    parts = s.split()
    tool = os.environ["TOOL"]
    if tool == "ib_send_lat" and len(parts) >= 9:
        parsed = {
            "bytes": int(parts[0]),
            "iterations": int(parts[1]),
            "t_min_usec": float(parts[2]),
            "t_max_usec": float(parts[3]),
            "t_typical_usec": float(parts[4]),
            "t_avg_usec": float(parts[5]),
            "t_stdev_usec": float(parts[6]),
            "p99_usec": float(parts[7]),
            "p999_usec": float(parts[8]),
        }
        break
    if tool == "ib_send_bw" and len(parts) >= 5:
        parsed = {
            "bytes": int(parts[0]),
            "iterations": int(parts[1]),
            "bw_peak_mib_s": float(parts[2]),
            "bw_avg_mib_s": float(parts[3]),
            "msg_rate_mpps": float(parts[4]),
        }
        break
row = {
    "role": "send_baseline",
    "tool": os.environ["TOOL"],
    "payload": int(os.environ["SIZE"]),
    "depth": int(os.environ["DEPTH"]),
    "repeat": int(os.environ["REP"]),
    "client_rc": int(os.environ["RC"]),
    "parsed": parsed,
    "raw": os.environ["TAG"] + ".client",
    "completion": "ibv_send",
    "status": "ok" if os.environ["RC"] == "0" and parsed else "error",
}
open(os.path.join(out, "results.jsonl"), "a").write(json.dumps(row) + "\n")
PY
    [[ $rc -eq 0 ]]
}

FAIL=0
sizes_lat=(64 256 1024 4096 16384)
sizes_bw=(65536 131072)
depths=(1 4 16 64)
for ((rep = 1; rep <= REPEATS; rep++)); do
    for sz in "${sizes_lat[@]}"; do
        run_one ib_send_lat "$sz" 1 "$rep" || FAIL=1
    done
    for sz in "${sizes_bw[@]}"; do
        for d in "${depths[@]}"; do
            run_one ib_send_bw "$sz" "$d" "$rep" || FAIL=1
        done
    done
done

if [[ $FAIL -ne 0 ]]; then
    echo "SEND baseline recorded errors in $OUT" >&2
    exit 1
fi
echo "wrote $OUT/config.json $OUT/results.jsonl"
