#!/bin/bash
# Paired TCP / RDMA echo. Does not start efsd and does not change a mount.
# Ports 19810 and 19820 are refused. Echo completion is not an fsync.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${EFS_XPRT_BIN:-$HERE/xprt_bench}
PORT=${EFS_XPRT_PORT:-19960}
BIND=${EFS_XPRT_BIND:-0.0.0.0}
HOST=${EFS_XPRT_HOST:-127.0.0.1}
REPEATS=${EFS_XPRT_REPEATS:-5}
QUICK=${EFS_XPRT_QUICK:-0}
SERVER_ALREADY=${EFS_XPRT_SERVER_ALREADY:-0}
RDMA_BUFS=${EFS_RDMA_BUFS:-64}

if [[ $# -ne 1 ]]; then
    echo "usage: $0 OUTDIR" >&2
    exit 2
fi
OUT=$1
mkdir -p "$OUT"

case "$PORT" in
    19810|19820)
        echo "refusing port $PORT — this bench does not use the live cluster" >&2
        exit 2
        ;;
esac
if [[ "$REPEATS" -lt 5 && "$QUICK" != 1 ]]; then
    echo "repeats must be at least 5 (or set EFS_XPRT_QUICK=1 for a wiring check)" >&2
    exit 2
fi
if [[ ! -x "$BIN" ]]; then
    cat >&2 <<EOF
xprt_bench is not built ($BIN).

Build it on an fcstor, in /tmp/efs, after rsync. Do not run make in
\$HOME/git/efs: the login node is AVX-512 and the fcstors are Zen2.

  rsync -a --delete --exclude='/mnt/' --exclude='*.log' "\$HOME/git/efs/" /tmp/efs/
  cd /tmp/efs && make tests/perf/tcp_rdma/xprt_bench
  bash tests/perf/tcp_rdma/run_xprt.sh /tmp/tcp-rdma-out
EOF
    exit 2
fi

if [[ "$QUICK" == 1 ]]; then
    REPEATS=1
fi
export LC_ALL=C
VER=$("$BIN" version)
export VER HOST PORT BIND REPEATS QUICK OUT RDMA_BUFS
python3 - <<'PY'
import datetime, json, os, socket, subprocess
ver = json.loads(os.environ["VER"])
def sh(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True,
                                        stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, OSError):
        return ""
ib = ""
try:
    ib = " ".join(sorted(os.listdir("/sys/class/infiniband")))
except OSError:
    pass
host = os.environ["HOST"]
cfg = {
    "bench": "xprt_bench",
    "completion": "transport_echo",
    "not_filesystem_durability": True,
    "live_cluster": "not used; ports 19810 and 19820 are refused",
    "placement": "loopback" if host in ("127.0.0.1", "localhost") else host,
    "host": host,
    "bind": os.environ["BIND"],
    "port": int(os.environ["PORT"]),
    "repeats": int(os.environ["REPEATS"]),
    "quick": os.environ["QUICK"] == "1",
    "hostname": socket.gethostname(),
    "uname": sh("uname -s -m"),
    "nproc": sh("nproc"),
    "cpu_model": sh("awk -F: '/[Mm]odel name/ {gsub(/^[ \\t]+/, \"\", $2); print $2; exit}' /proc/cpuinfo"),
    "ib_devices": ib,
    "started_utc": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
    "binary": ver,
    "EFS_RDMA_BUFS": os.environ["RDMA_BUFS"],
    "note": "TCP and RDMA use this same binary, host, payload, and depth. "
            "The transport flag is the only switch. A loopback placement is "
            "a same-host QP, not a cross-host result.",
}
with open(os.path.join(os.environ["OUT"], "config.json"), "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PY

: >"$OUT/results.jsonl"
SERIES=0
FAIL=0
SRV_PID=

cleanup() {
    if [[ -n "${SRV_PID:-}" ]]; then
        kill "$SRV_PID" 2>/dev/null || true
        wait "$SRV_PID" 2>/dev/null || true
        SRV_PID=
    fi
}
trap cleanup EXIT

crash_line() {
    local why=$1
    printf '{"role":"client","transport":"%s","mode":"%s","phase":"%s","payload":%s,"depth":%s,"clients":%s,"repeat":%s,"idle_ms":%s,"series":%s,"status":"crash","errors":1,"timeouts":0,"fallbacks":0,"verified":false,"completion":"transport_echo","why":"%s"}\n' \
        "$transport" "$mode" "$phase" "$payload" "$depth" "$clients" \
        "$repeat" "$idle" "$SERIES" "$why" >>"$OUT/results.jsonl"
}

run_case() {
    local transport=$1 mode=$2 payload=$3 depth=$4 phase=$5 repeat=$6
    local idle=$7 clients=$8 samples=$9 warmup=${10}
    SERIES=$((SERIES + 1))
    local ready=$OUT/ready-$SERIES
    local stats=$OUT/server-$SERIES.json
    local tmp=$OUT/work-$SERIES
    rm -rf "$tmp"
    mkdir -p "$tmp"
    rm -f "$ready" "$stats"

    if [[ "$SERVER_ALREADY" != 1 ]]; then
        EFS_RDMA_BUFS=$RDMA_BUFS \
            "$BIN" server --bind "$BIND" --port "$PORT" --expect "$clients" \
            --ready "$ready" --stats "$stats" --series "$SERIES" \
            >"$OUT/server-$SERIES.err" 2>&1 &
        SRV_PID=$!
        local ready_ok=0
        local i
        for i in $(seq 1 150); do
            if [[ -f $ready ]]; then
                ready_ok=1
                break
            fi
            if ! kill -0 "$SRV_PID" 2>/dev/null; then
                break
            fi
            sleep 0.02
        done
        if [[ $ready_ok -ne 1 ]]; then
            echo "server did not become ready for series $SERIES" >&2
            crash_line server_not_ready
            cleanup
            FAIL=1
            return
        fi
    fi

    local i extra=()
    if [[ "$samples" -ge 0 ]]; then
        extra+=(--samples "$samples")
    fi
    for ((i = 0; i < clients; i++)); do
        EFS_TRANSPORT=$transport EFS_RDMA_BUFS=$RDMA_BUFS \
            "$BIN" client --host "$HOST" --port "$PORT" \
            --transport "$transport" --mode "$mode" --payload "$payload" \
            --depth "$depth" --phase "$phase" --repeat "$repeat" \
            --idle-ms "$idle" --clients "$clients" --client-index "$i" \
            --series "$SERIES" --warmup "$warmup" "${extra[@]}" \
            >"$tmp/$i.json" 2>>"$OUT/client-$SERIES.err" &
        echo $! >>"$tmp/pids"
    done
    local p
    while read -r p; do
        if ! wait "$p"; then
            FAIL=1
        fi
    done <"$tmp/pids"

    for ((i = 0; i < clients; i++)); do
        if [[ -s $tmp/$i.json ]]; then
            cat "$tmp/$i.json" >>"$OUT/results.jsonl"
        else
            crash_line no_client_json
            FAIL=1
        fi
    done

    if [[ -n "${SRV_PID:-}" ]]; then
        if ! wait "$SRV_PID"; then
            FAIL=1
        fi
        SRV_PID=
    fi
    if [[ -s $stats ]]; then
        cat "$stats" >>"$OUT/results.jsonl"
    fi
    rm -rf "$tmp" "$ready"
}

if [[ "$QUICK" == 1 ]]; then
    LAT_SIZES=(64)
    TP_SIZES=(65536)
    TP_DEPTHS=(1)
    IDLE_SIZES=(64)
    IDLE_GAPS=(0)
    MULTI_CLIENTS=1
    LAT_SAMPLES=200
    LAT_WARMUP=20
    TP_SAMPLES=64
    TP_WARMUP=1
    IDLE_SAMPLES=5
    IDLE_WARMUP=1
    REPEATS=1
else
    # 251: largest payload whose 5-byte frame is still an inline SEND (256).
    LAT_SIZES=(64 251 256 1024 4096 16384)
    TP_SIZES=(65536 131072)
    TP_DEPTHS=(1 4 16 64)
    IDLE_SIZES=(64 4096)
    IDLE_GAPS=(0 1 10 100 1000)
    MULTI_CLIENTS=4
    LAT_SAMPLES=5000
    LAT_WARMUP=200
    TP_SAMPLES=0
    TP_WARMUP=1
    IDLE_SAMPLES=40
    IDLE_WARMUP=2
fi

# --samples 0 is not accepted. Throughput's unset count is the binary
# default (about 256 MiB). Quick mode passes an explicit small count.
tp_sample_arg() {
    if [[ "$TP_SAMPLES" == 0 ]]; then
        echo -1
    else
        echo "$TP_SAMPLES"
    fi
}

run_matrix() {
    local phase=$1 reps=$2
    local rep transport sz depth gap
    for ((rep = 1; rep <= reps; rep++)); do
        for transport in tcp rdma; do
            for sz in "${LAT_SIZES[@]}"; do
                run_case "$transport" latency "$sz" 1 "$phase" "$rep" 0 1 \
                    "$LAT_SAMPLES" "$LAT_WARMUP"
            done
            for sz in "${TP_SIZES[@]}"; do
                for depth in "${TP_DEPTHS[@]}"; do
                    run_case "$transport" throughput "$sz" "$depth" "$phase" \
                        "$rep" 0 1 "$(tp_sample_arg)" "$TP_WARMUP"
                done
            done
            if [[ "$MULTI_CLIENTS" -gt 1 ]]; then
                run_case "$transport" throughput 65536 1 "$phase" "$rep" 0 \
                    "$MULTI_CLIENTS" "$(tp_sample_arg)" "$TP_WARMUP"
            fi
            for sz in "${IDLE_SIZES[@]}"; do
                for gap in "${IDLE_GAPS[@]}"; do
                    run_case "$transport" idle "$sz" 1 "$phase" "$rep" "$gap" \
                        1 "$IDLE_SAMPLES" "$IDLE_WARMUP"
                done
            done
        done
    done
}

# One discarded pair, then the measured repeats. Inside each repeat, TCP
# runs and then RDMA runs, so the two transports alternate.
run_matrix warmup 1
run_matrix measure "$REPEATS"

set +e
python3 "$HERE/summarize.py" "$OUT/results.jsonl" "$OUT/summary.json" \
    | tee "$OUT/summary.txt"
sum_rc=${PIPESTATUS[0]}
set -e
if [[ $sum_rc -ne 0 ]]; then
    FAIL=1
fi
if [[ $FAIL -ne 0 ]]; then
    echo "tcp/rdma bench recorded failures in $OUT" >&2
    exit 1
fi
echo "wrote $OUT/config.json $OUT/results.jsonl $OUT/summary.json"
