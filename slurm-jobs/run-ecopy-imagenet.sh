#!/bin/bash
# Bring up 3×4-disk IB servers + one ecopy client for imagenet.
# Usage:
#   ./slurm-jobs/run-ecopy-imagenet.sh
#   SRC=/path ITER=1 ./slurm-jobs/run-ecopy-imagenet.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

ITER="${ITER:-1}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-04:00:00}"
EXPORT_NAME="${EXPORT_NAME:-efs-imagenet}"
SRC="${SRC:-/home/erbmi1/orcd/scratch/imagenet}"
NUM_SERVERS=3
EFS_DIO="${EFS_DIO:-on}"
EFS_QUOTA="${EFS_QUOTA:-200G}"
cd "$REPO"

PROF_ROOT="$SHARED/logs/ecopy-i${ITER}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF_ROOT" "$SHARED/state" "$SHARED/logs"
echo "$PROF_ROOT" > "$SHARED/logs/ecopy-last-dir"
echo "=== ecopy imagenet iter=$ITER PROF=$PROF_ROOT ==="
echo "SRC=$SRC EXPORT=$EXPORT_NAME"

echo "Building..."
sbatch --wait -p mit_quicktest --time=00:15:00 slurm-jobs/build.sh
echo "Build done"

# Cancel leftover harness jobs
PREV=$(squeue -h -u "$USER" -o '%i %j' 2>/dev/null | awk '/efs-s4d|efs-ecopy|efs-mkfs/{print $1}' || true)
if [ -n "$PREV" ]; then
    echo "cancelling prior: $PREV"
    echo "$PREV" | xargs -r scancel || true
    sleep 3
fi
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in $(seq 1 "$NUM_SERVERS"); do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
        -J "efs-s4d-${sid}" "${excl_args[@]}" \
        --export=ALL,SERVER_ID="$sid",EFS_DIO="$EFS_DIO",EFS_QUOTA="$EFS_QUOTA" \
        slurm-jobs/server-4disk.sh)
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
    echo "  s${sid} job $job on $node"
done

efs_assert_distinct_servers "$NUM_SERVERS"
echo "${SERVER_NODES[*]}" | tee "$PROF_ROOT/server_nodes.txt"

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s4d-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 90); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
    echo "--- s job $job ---"
    grep -E 'listening on|Joined|writers=|paths' "$log" | tail -5 || true
done
sleep 3

for i in $(seq 1 "$NUM_SERVERS"); do
    addr=$(cat "$SHARED/state/s${i}.addr")
    host=${addr%:*}
    port=${addr#*:}
    ok=0
    for _ in $(seq 1 30); do
        if timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; then ok=1; break; fi
        sleep 1
    done
    [ "$ok" = 1 ] || { echo "ERROR: $addr not reachable"; exit 1; }
done

MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME" slurm-jobs/mkfs-ib.sh)
echo "mkfs job $MKFS"
while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 5; done
cat "$SHARED/logs/mkfs-${MKFS}.out" 2>/dev/null || true
sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED || {
    echo "mkfs failed"; exit 1;
}

CLIENT=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --cpus-per-task=16 --mem=32G --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME",SRC="$SRC",PROF_ROOT="$PROF_ROOT",NUM_SERVERS="$NUM_SERVERS",EFS_META_BATCH_OPS=65536 \
    slurm-jobs/client-ecopy-imagenet.sh)
echo "client job $CLIENT (exclude=$EXCLUDE)"
echo "$CLIENT" > "$PROF_ROOT/client_job"
echo "${SERVER_JOBS[*]}" > "$PROF_ROOT/server_jobs"

echo "Waiting for client $CLIENT (poll every 60s)..."
while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do
    sleep 60
    st=$(squeue -h -j "$CLIENT" -o '%T %M' 2>/dev/null || echo done)
    echo "  $(date +%H:%M:%S) client=$st"
done

echo "=== client sacct ==="
sacct -j "$CLIENT" --format=JobID,State,ExitCode,Elapsed,MaxRSS -n || true
echo "=== RESULT ==="
cat "$PROF_ROOT/RESULT" 2>/dev/null || echo "(no RESULT file)"
echo "=== fuse log tail ==="
tail -60 "$PROF_ROOT"/fuse-*.log 2>/dev/null || true
echo "=== ecopy log tail ==="
tail -40 "$PROF_ROOT"/ecopy-*.log 2>/dev/null || true
echo "=== client job out tail ==="
tail -80 "$SHARED/logs/ecopy-${CLIENT}.out" 2>/dev/null || true

if grep -q '^PASS' "$PROF_ROOT/RESULT" 2>/dev/null; then
    echo "ITERATION $ITER PASS"
    # tear down servers
    echo "${SERVER_JOBS[*]}" | xargs -r scancel || true
    exit 0
fi
echo "ITERATION $ITER FAIL"
# leave servers briefly for log inspection? cancel to free nodes
echo "${SERVER_JOBS[*]}" | xargs -r scancel || true
exit 1
