#!/bin/bash
# One optimization cycle: local/net/store bench matrix + client/server perf.
#
# Usage:
#   CYCLE=1 PARTITION=mit_normal ./slurm-jobs/run-bench-opt-cycle.sh
#
# Outputs under:
#   /orcd/scratch/.../efs/profile/bench-opt-c${CYCLE}-<timestamp>/

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
CYCLE="${CYCLE:-1}"
PARTITION="${PARTITION:-mit_normal}"
NET_TIME="${NET_TIME:-5}"
STORE_SIZE="${STORE_SIZE:-1G}"
LOCAL_TIME="${LOCAL_TIME:-5}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

PROF="$SHARED/profile/bench-opt-c${CYCLE}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF" "$SHARED/logs" "$SHARED/state" "$SHARED/profile"
LEDGER="$PROF/ledger.tsv"
printf 'combo\tmetric\tvalue\tnotes\n' > "$LEDGER"

echo "=== bench-opt cycle=$CYCLE PROF=$PROF ==="

echo "Building..."
BUILD=$(sbatch --parsable --wait -p "$PARTITION" --time=00:15:00 --cpus-per-task=4 --mem=4G \
    slurm-jobs/build.sh)
echo "Build: $BUILD"

record() {
    printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$LEDGER"
    echo "LEDGER $1 $2=$3 ($4)"
}

# --- A) local path benches ---
for writers in 8 16; do
    for dio in on off; do
        name="local_w${writers}_dio${dio}"
        echo "=== $name ==="
        job=$(sbatch --parsable --wait -p "$PARTITION" --time=00:15:00 \
            --cpus-per-task=4 --mem=8G \
            --export=ALL,TIME_SEC="$LOCAL_TIME",WRITERS="$writers",EFS_DIO="$dio" \
            slurm-jobs/server-bench.sh)
        out=$(ls -t "$SHARED/logs"/server-bench-"${job}".out 2>/dev/null | head -1 || true)
        mkdir -p "$PROF/$name"
        if [ -n "$out" ]; then
            cp -f "$out" "$PROF/$name/out.txt"
            line=$(grep 'BENCH_OK kind=local' "$out" | tail -1 || true)
            echo "$line" | tee "$PROF/$name/summary.txt"
            gib=$(echo "$line" | sed -n 's/.*GiB_s=\([0-9.]*\).*/\1/p')
            record "$name" "GiB_s" "${gib:-0}" "$line"
        else
            record "$name" "GiB_s" "FAIL" "no output"
        fi
    done
done

# --- B) 3-server cluster for net + store (+perf) ---
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in 1 2 3; do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    # Profile server1 only
    extra=""
    export_extra="ALL,EFS_DIO=off"
    if [ "$sid" = 1 ]; then
        export_extra="ALL,EFS_DIO=off,EFS_EXTRA_ARGS=--perf,EFS_PERF_PATH=${PROF}/server1.perf.data"
    fi
    # --no-requeue: a mid-run requeue kills efsd but leaves stale state/*.addr
    job=$(sbatch --parsable -p "$PARTITION" --time=00:45:00 --no-requeue \
        --cpus-per-task=4 --mem=8G "${excl_args[@]}" \
        --export="${export_extra}" "slurm-jobs/server${sid}.sh")
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
    echo "  s${sid}=$node job=$job"
done
efs_assert_distinct_servers 3
echo "${SERVER_NODES[*]}" > "$PROF/server_nodes.txt"

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 90); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
done
sleep 3

servers_alive() {
    local i addr host port
    for i in 1 2 3; do
        [ -f "$SHARED/state/s${i}.addr" ] || return 1
        addr=$(cat "$SHARED/state/s${i}.addr")
        host=${addr%:*}
        port=${addr#*:}
        timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null || return 1
    done
    return 0
}

run_client() {
    local name=$1
    shift
    echo "=== $name ==="
    mkdir -p "$PROF/$name"
    if ! servers_alive; then
        echo "ERROR: servers not reachable before $name" | tee "$PROF/$name/summary.txt"
        record "$name" "result" "FAIL" "servers down"
        return 0
    fi
    local exp="ALL,NUM_SERVERS=3,PROF_DIR=${PROF}/${name}"
    local kv
    for kv in "$@"; do
        exp="${exp},${kv}"
    done
    local job=""
    set +e
    job=$(sbatch --parsable --wait -p "$PARTITION" --time=00:30:00 \
        --cpus-per-task=8 --mem=16G --exclude="$EXCLUDE" \
        --export="$exp" \
        slurm-jobs/client-bench.sh)
    local rc=$?
    set -e
    out=$(ls -t "$SHARED/logs"/client-bench-"${job}".out 2>/dev/null | head -1 || true)
    if [ -n "$out" ]; then
        cp -f "$out" "$PROF/$name/out.txt"
        line=$(grep 'BENCH_OK' "$out" | tail -1 || true)
        echo "$line" | tee "$PROF/$name/summary.txt"
        if echo "$line" | grep -q 'kind=net'; then
            gib=$(echo "$line" | sed -n 's/.*GiB_s=\([0-9.]*\).*/\1/p')
            record "$name" "GiB_s" "${gib:-0}" "$line"
        elif echo "$line" | grep -q 'kind=store'; then
            lg=$(echo "$line" | sed -n 's/.*logical_GiB_s=\([0-9.]*\).*/\1/p')
            sg=$(echo "$line" | sed -n 's/.*stored_GiB_s=\([0-9.]*\).*/\1/p')
            record "$name" "logical_GiB_s" "${lg:-0}" "$line"
            record "$name" "stored_GiB_s" "${sg:-0}" "$line"
        else
            record "$name" "result" "FAIL" "rc=$rc ${line:-no BENCH_OK}"
        fi
        [ -f "$PROF/$name/client.hotpath.txt" ] && \
            head -30 "$PROF/$name/client.hotpath.txt" || true
    else
        record "$name" "result" "FAIL" "no output rc=$rc"
    fi
}

# Net discard matrix
run_client "net_t${NET_TIME}" MODE=net TIME_SEC="$NET_TIME"
run_client "net_t${NET_TIME}_perf" MODE=net TIME_SEC="$NET_TIME" EFS_BENCH_PERF=1

# Store matrix
for sz in 512M 1G 2G; do
    run_client "store_${sz}" MODE=store SIZE="$sz"
done
run_client "store_${STORE_SIZE}_perf" MODE=store SIZE="$STORE_SIZE" EFS_BENCH_PERF=1

# Conn pool variant
run_client "store_1G_conns16" MODE=store SIZE=1G EFS_CLIENT_CONNS_PER_NODE=16
run_client "store_1G_conns32" MODE=store SIZE=1G EFS_CLIENT_CONNS_PER_NODE=32

# Collect server1 perf if present
if [ -f "$PROF/server1.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.4 -i "$PROF/server1.perf.data" \
        > "$PROF/server1.report.txt" 2>/dev/null || true
    {
        echo "=== server1 hotspots ==="
        grep -E 'blake3|efs_|memcpy|memmove|write|hash|put_|send|recv' \
            "$PROF/server1.report.txt" || true
        echo "--- top ---"
        grep -E '^\s+[0-9]+\.[0-9]+%' "$PROF/server1.report.txt" | head -20 || true
    } | tee "$PROF/server1.hotpath.txt"
fi

# Copy client hotpaths into PROF
find "$PROF" -name '*.hotpath.txt' -o -name 'client.perf.data*' 2>/dev/null | head -40 || true
for d in "$PROF"/net_*_perf "$PROF"/store_*_perf; do
    [ -d "$d" ] || continue
    # efs-bench may have written beside EFS_PERF_PATH
    for f in "$d".hotpath.txt "$d/client.perf.data.hotpath.txt" \
             "${d}/client.perf.data.hotpath.txt"; do
        [ -f "$f" ] && cp -f "$f" "$d/hotpath.txt" 2>/dev/null || true
    done
    ls -la "$d" 2>/dev/null || true
done
# Search shared logs / orcd for hotpath near this run
find "$PROF" -type f -name '*hotpath*' 2>/dev/null | tee "$PROF/hotpath_files.txt" || true

for job in "${SERVER_JOBS[@]}"; do
    scancel "$job" 2>/dev/null || true
done
sleep 2
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

{
    echo "CYCLE=${CYCLE} COMPLETE $(date -Is)"
    echo "PROF=$PROF"
    column -t -s $'\t' "$LEDGER" 2>/dev/null || cat "$LEDGER"
    echo
    echo "=== hotpath snippets ==="
    for f in "$PROF"/server1.hotpath.txt "$PROF"/*/*.hotpath.txt "$PROF"/*/*hotpath*; do
        [ -f "$f" ] || continue
        echo "--- $f ---"
        head -40 "$f"
    done
} | tee "$PROF/FINAL.txt"

echo "DONE cycle=$CYCLE PROF=$PROF"
