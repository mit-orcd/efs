#!/bin/bash
# 3×4-disk IB cluster + one client: store-bench + single dd write/read + perf.
#
# Usage:
#   LABEL=baseline EFS_DIO=off EFS_FUSE_PERF=1 ./slurm-jobs/run-dd-bw-4disk.sh
#   LABEL=dio EFS_DIO=on FILE_GIB=4 ./slurm-jobs/run-dd-bw-4disk.sh
set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-01:00:00}"
FILE_GIB="${FILE_GIB:-4}"
EFS_DIO="${EFS_DIO:-off}"
LABEL="${LABEL:-ddbw}"
NUM_SERVERS=3
EXPORT_NAME="ddbw-${LABEL}-$$"
SKIP_BUILD="${SKIP_BUILD:-0}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

PROF="$SHARED/profile/dd-bw-${LABEL}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF" "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
chmod +x "$REPO/slurm-jobs/client-dd-bw.sh"

cleanup_jobs() {
    set +e
    for j in "$@"; do
        [ -n "$j" ] && scancel "$j" 2>/dev/null || true
    done
}

echo "=== dd-bw label=$LABEL dio=$EFS_DIO file_gib=$FILE_GIB PROF=$PROF ==="

if [ "$SKIP_BUILD" != "1" ]; then
    sbatch --wait -p "$PARTITION" --time=00:15:00 slurm-jobs/build.sh
fi

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
rm -f "$PROF/GO"

PREV=$(squeue -u erbmi1 -h -o '%i %j' 2>/dev/null | awk '/efs-s4d|efs-dd-bw|efs-mkfs/{print $1}' || true)
[ -n "$PREV" ] && echo "$PREV" | xargs -r scancel || true
sleep 2

SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in $(seq 1 "$NUM_SERVERS"); do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    export_extra="ALL,SERVER_ID=$sid,EFS_DIO=$EFS_DIO,EFS_QUOTA=100G"
    if [ "$sid" = 1 ]; then
        export_extra="${export_extra},EFS_EXTRA_ARGS=--perf,EFS_PERF_PATH=${PROF}/server1.perf.data"
    fi
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" --no-requeue \
        --cpus-per-task=8 --mem=32G "${excl_args[@]}" \
        --export="$export_extra" slurm-jobs/server-4disk.sh)
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    node=$(cat "$SHARED/state/s${sid}.node")
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
    echo "  s${sid} job=$job node=$node"
done
echo "servers: ${SERVER_NODES[*]}" | tee "$PROF/server_nodes.txt"

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s4d-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 90); do
        grep -q "listening on" "$log" 2>/dev/null && break
        squeue -h -j "$job" >/dev/null || { echo "server $job died"; tail -30 "$log"; exit 1; }
        sleep 2
    done
    grep -E 'listening on|local_ec|Joined' "$log" | tail -5 || true
done
sleep 3

MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME" slurm-jobs/mkfs-ib.sh)
echo "mkfs job=$MKFS"
while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 60; done
sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED || {
    cat "$SHARED/logs/mkfs-${MKFS}.out" || true
    cleanup_jobs "${SERVER_JOBS[@]}"
    exit 1
}

CLIENT=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --cpus-per-task=8 --mem=16G --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF",NUM_SERVERS="$NUM_SERVERS",FILE_GIB="$FILE_GIB",DO_STORE_BENCH=1,EFS_FUSE_PERF="${EFS_FUSE_PERF:-1}",EFS_BENCH_PERF="${EFS_BENCH_PERF:-0}",LABEL="$LABEL" \
    slurm-jobs/client-dd-bw.sh)
echo "client job=$CLIENT"
touch "$PROF/GO"

while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do
    sleep 60
    echo "  $(date +%H:%M:%S) $(squeue -h -j "$CLIENT" -o '%T %M %N' 2>/dev/null || true)"
done

# Collect server1 perf
if [ -f "$PROF/server1.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/server1.perf.data" \
        >"$PROF/server1.report.txt" 2>/dev/null || true
    {
        echo "=== server1 hotspots ==="
        grep -E 'blake3|efs_|memcpy|write|fsync|encode|local_ec|shard' \
            "$PROF/server1.report.txt" || true
        echo "--- top ---"
        grep -E '^\s+[0-9]+\.[0-9]+%' "$PROF/server1.report.txt" | head -25 || true
    } | tee "$PROF/server1.hotpath.txt"
fi

cleanup_jobs "${SERVER_JOBS[@]}"
sleep 2
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

{
    echo "LABEL=$LABEL EFS_DIO=$EFS_DIO FILE_GIB=$FILE_GIB"
    echo "PROF=$PROF"
    echo "servers=${SERVER_NODES[*]}"
    cat "$PROF"/client-*/SUMMARY.txt 2>/dev/null || true
    echo
    echo "=== client hotpath ==="
    head -40 "$PROF"/client-*/client.hotpath.txt 2>/dev/null || true
    echo "=== server1 hotpath ==="
    head -40 "$PROF/server1.hotpath.txt" 2>/dev/null || true
} | tee "$PROF/FINAL.txt"

echo "DONE PROF=$PROF"
grep -E 'write_GiB_s|read_GiB_s|BENCH_OK|logical_GiB' "$PROF/FINAL.txt" || true
