#!/bin/bash
# Smoke: local path bench + 3srv network bench (short timed runs).
# Usage: ./slurm-jobs/smoke-bench.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PARTITION="${PARTITION:-mit_quicktest}"
TIME_SEC="${TIME_SEC:-3}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

mkdir -p "$SHARED/logs" "$SHARED/state" "$SHARED/profile"
PROF="$SHARED/profile/smoke-bench-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF"

echo "Building..."
BUILD=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 --cpus-per-task=4 --mem=4G \
    slurm-jobs/build.sh)
echo "Build job: $BUILD"

echo "=== local path bench ==="
LOCAL_JOB=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 \
    --cpus-per-task=4 --mem=8G \
    --export=ALL,TIME_SEC="$TIME_SEC",WRITERS=8 \
    slurm-jobs/server-bench.sh)
echo "local bench job: $LOCAL_JOB"
LOCAL_OUT=$(ls -t "$SHARED/logs"/server-bench-"${LOCAL_JOB}".out 2>/dev/null | head -1 || true)
if [ -n "$LOCAL_OUT" ]; then
    cp -f "$LOCAL_OUT" "$PROF/local.out"
    grep -E 'BENCH_OK kind=local' "$LOCAL_OUT" | tee "$PROF/local.summary" || {
        echo "FAIL: no BENCH_OK local"
        exit 1
    }
fi

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

echo "=== 3-server cluster + net bench ==="
SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in 1 2 3; do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    job=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
        --cpus-per-task=2 --mem=4G "${excl_args[@]}" \
        --export=ALL,EFS_DIO=on "slurm-jobs/server${sid}.sh")
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 300
    node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then
        EXCLUDE="$node"
    else
        EXCLUDE="${EXCLUDE},${node}"
    fi
    echo "  s${sid} job=$job node=$node"
done
efs_assert_distinct_servers 3

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 60); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
done
sleep 3

CLI=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 \
    --cpus-per-task=8 --mem=8G --exclude="$EXCLUDE" \
    --export=ALL,MODE=net,TIME_SEC="$TIME_SEC",NUM_SERVERS=3 \
    slurm-jobs/client-bench.sh)
echo "client net bench job: $CLI"
CLI_OUT=$(ls -t "$SHARED/logs"/client-bench-"${CLI}".out 2>/dev/null | head -1 || true)
if [ -n "$CLI_OUT" ]; then
    cp -f "$CLI_OUT" "$PROF/net.out"
    grep -E 'BENCH_OK kind=net' "$CLI_OUT" | tee "$PROF/net.summary" || {
        echo "FAIL: no BENCH_OK net"
        for job in "${SERVER_JOBS[@]}"; do scancel "$job" 2>/dev/null || true; done
        exit 1
    }
    if ! grep -E 'per_server_bytes:.*id1=.*id2=.*id3=' "$CLI_OUT"; then
        echo "WARN: expected per_server_bytes for id1/id2/id3"
    fi
fi

STORE_SIZE="${STORE_SIZE:-64M}"
STORE=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 \
    --cpus-per-task=8 --mem=8G --exclude="$EXCLUDE" \
    --export=ALL,MODE=store,SIZE="$STORE_SIZE",NUM_SERVERS=3 \
    slurm-jobs/client-bench.sh)
echo "client store bench job: $STORE"
STORE_OUT=$(ls -t "$SHARED/logs"/client-bench-"${STORE}".out 2>/dev/null | head -1 || true)
if [ -n "$STORE_OUT" ]; then
    cp -f "$STORE_OUT" "$PROF/store.out"
    grep -E 'BENCH_OK kind=store' "$STORE_OUT" | tee "$PROF/store.summary" || {
        echo "FAIL: no BENCH_OK store"
        for job in "${SERVER_JOBS[@]}"; do scancel "$job" 2>/dev/null || true; done
        exit 1
    }
fi

for job in "${SERVER_JOBS[@]}"; do
    scancel "$job" 2>/dev/null || true
done
sleep 2
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

{
    echo "SMOKE_BENCH_OK"
    echo "PROF=$PROF"
    cat "$PROF/local.summary" 2>/dev/null || true
    cat "$PROF/net.summary" 2>/dev/null || true
    cat "$PROF/store.summary" 2>/dev/null || true
} | tee "$PROF/FINAL.txt"

echo "=== done ==="
cat "$PROF/FINAL.txt"
