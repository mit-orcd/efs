#!/bin/bash
# 3×4-disk ASAN efsd cluster + ecopy of ImageNet val (6.4G / 50k files).
# Faster crash repro than full imagenet. Usage: ./slurm-jobs/run-ecopy-asan-val.sh
set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

ITER="${ITER:-asan-val}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-01:00:00}"
EXPORT_NAME="${EXPORT_NAME:-efs-asan-val}"
SRC="${SRC:-/home/erbmi1/orcd/scratch/imagenet/images_complete/ilsvrc/val}"
NUM_SERVERS=3
EFS_DIO="${EFS_DIO:-off}"
EFS_QUOTA="${EFS_QUOTA:-200G}"
cd "$REPO"

PROF_ROOT="$SHARED/logs/ecopy-i${ITER}-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF_ROOT" "$SHARED/state" "$SHARED/logs"
echo "$PROF_ROOT" > "$SHARED/logs/ecopy-last-dir"
echo "=== ASAN val ecopy PROF=$PROF_ROOT ==="

echo "Building ASAN efsd + normal fuse..."
sbatch --wait -p mit_quicktest --time=00:15:00 --wrap="
set -e
cd $REPO
make clean
make -j4 efs-fuse efs-mgmt
make -j4 efsd \
  CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-stringop-truncation -Wno-format-truncation -fsanitize=address' \
  LDFLAGS='-fsanitize=address -lpthread -lm -ldl'
nm efsd | grep -q __asan_init
ls -la efsd efs-fuse
" -o "$SHARED/logs/build-asan-val-%j.out" -e "$SHARED/logs/build-asan-val-%j.err"
echo "ASAN build done"

PREV=$(squeue -h -u "$USER" -o '%i %j' 2>/dev/null | awk '/efs-s4d|efs-ecopy|efs-mkfs|efs-asan/{print $1}' || true)
if [ -n "$PREV" ]; then
    echo "cancelling prior: $PREV"
    echo "$PREV" | xargs -r scancel || true
    sleep 3
fi
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=0:halt_on_error=1:print_stacktrace=1}"

SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in $(seq 1 "$NUM_SERVERS"); do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" --mem=48G \
        -J "efs-s4d-${sid}" "${excl_args[@]}" \
        --export=ALL,SERVER_ID="$sid",EFS_DIO="$EFS_DIO",EFS_QUOTA="$EFS_QUOTA",ASAN_OPTIONS="$ASAN_OPTIONS" \
        slurm-jobs/server-4disk.sh)
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    if [ -s "$SHARED/state/s${sid}.node" ]; then
        node=$(cat "$SHARED/state/s${sid}.node")
    else
        node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    fi
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
    echo "  s${sid} job $job on $node"
done

echo "${SERVER_NODES[*]}" | tee "$PROF_ROOT/server_nodes.txt"
echo "server nodes: ${SERVER_NODES[*]}"
for i in $(seq 0 $((NUM_SERVERS - 1))); do
    for j in $(seq $((i + 1)) $((NUM_SERVERS - 1))); do
        if [ "${SERVER_NODES[$i]}" = "${SERVER_NODES[$j]}" ]; then
            echo "ERROR: duplicate server node ${SERVER_NODES[$i]}" >&2
            exit 1
        fi
    done
done

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s4d-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 90); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
done
sleep 3

MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME" slurm-jobs/mkfs-ib.sh)
echo "mkfs job $MKFS"
while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 5; done
cat "$SHARED/logs/mkfs-${MKFS}.out" 2>/dev/null || true

CLIENT=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --cpus-per-task=16 --mem=32G --exclude="$EXCLUDE" \
    --export=ALL,EXPORT_NAME="$EXPORT_NAME",SRC="$SRC",PROF_ROOT="$PROF_ROOT",NUM_SERVERS="$NUM_SERVERS",EFS_META_BATCH_OPS=65536 \
    slurm-jobs/client-ecopy-imagenet.sh)
echo "client job $CLIENT"
echo "$CLIENT" > "$PROF_ROOT/client_job"
echo "${SERVER_JOBS[*]}" > "$PROF_ROOT/server_jobs"

echo "Waiting for client $CLIENT (poll every 60s)..."
while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do
    sleep 60
    st=$(squeue -h -j "$CLIENT" -o '%T %M' 2>/dev/null || echo done)
    echo "  $(date +%H:%M:%S) client=$st"
    # Early abort if any server died
    for job in "${SERVER_JOBS[@]}"; do
        state=$(sacct -j "$job" -n -o State -X 2>/dev/null | head -1 | tr -d ' ')
        if [ -n "$state" ] && [ "$state" != "RUNNING" ] && [ "$state" != "PENDING" ]; then
            echo "SERVER $job left RUNNING (state=$state) — collecting ASAN logs"
            scancel "$CLIENT" 2>/dev/null || true
            break 2
        fi
    done
done

echo "=== RESULT ==="
cat "$PROF_ROOT/RESULT" 2>/dev/null || echo "(no RESULT)"
for job in "${SERVER_JOBS[@]}"; do
    echo "=== s job $job err (asan/fatal) ==="
    grep -E 'AddressSanitizer|SUMMARY|efsd: fatal|SIGSEGV|heap-|stack-' \
        "$SHARED/logs/s4d-${job}.err" 2>/dev/null | head -40 || true
    tail -30 "$SHARED/logs/s4d-${job}.err" 2>/dev/null || true
done
echo "=== fuse tail ==="
tail -40 "$PROF_ROOT"/fuse-*.log 2>/dev/null || true
echo "=== ecopy tail ==="
tail -30 "$PROF_ROOT"/ecopy-*.log 2>/dev/null || true

# cleanup servers
echo "${SERVER_JOBS[*]}" | xargs -r scancel || true
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
echo "asan-val harness done PROF=$PROF_ROOT"
