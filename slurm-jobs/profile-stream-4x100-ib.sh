#!/bin/bash
# Multi-node IB stream: 2 clients × 2 × FILE_GIB GiB parallel writes (default 100).
# Cluster absorbs 4 × FILE_GIB GiB. Servers on 3 distinct nodes; clients on 2 more.
#
# Usage: ROUND=1 FILE_GIB=100 ./slurm-jobs/profile-stream-4x100-ib.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
FILE_GIB="${FILE_GIB:-100}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-2}"
NUM_CLIENTS="${NUM_CLIENTS:-2}"
PARTITION="${PARTITION:-mit_normal}"
EXPORT_NAME="stream4x${FILE_GIB}r${ROUND}"
# Servers hold ~1.5x EC; 400 GiB logical → budget several hours.
JOB_TIME="${JOB_TIME:-06:00:00}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

PROF_ROOT="$SHARED/profile/stream-4x100-ib-r${ROUND}"
rm -rf "$PROF_ROOT"
mkdir -p "$PROF_ROOT" "$SHARED/state" "$SHARED/logs" "$SHARED/profile"

echo "Building efs binaries via Slurm..."
BUILD_JOB=$(sbatch --parsable --wait slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
rm -f "$PROF_ROOT/GO"

PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-str-cli,efs-mkfs 2>/dev/null || true)
if [ -n "$PREV" ]; then
    echo "Cancelling previous jobs: $PREV"
    echo "$PREV" | xargs -r scancel || true
    sleep 3
fi

echo "=== round $ROUND: ${NUM_CLIENTS} clients × ${FILES_PER_CLIENT} × ${FILE_GIB} GiB over IB ==="
echo "Submitting IB servers on distinct nodes (partition=$PARTITION time=$JOB_TIME)..."
JOB1=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" slurm-jobs/server1.sh)
echo "  s1 job $JOB1"
efs_wait_addr 1 900
N1=$(efs_slurm_node "$(cat "$SHARED/state/s1.host")")
echo "  s1 on $N1 ($(cat "$SHARED/state/s1.addr"))"

JOB2=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --exclude="$N1" slurm-jobs/server2.sh)
echo "  s2 job $JOB2 (exclude=$N1)"
efs_wait_addr 2 900
N2=$(efs_slurm_node "$(cat "$SHARED/state/s2.host")")
echo "  s2 on $N2 ($(cat "$SHARED/state/s2.addr"))"

JOB3=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --exclude="${N1},${N2}" slurm-jobs/server3.sh)
echo "  s3 job $JOB3 (exclude=${N1},${N2})"
efs_wait_addr 3 900
N3=$(efs_slurm_node "$(cat "$SHARED/state/s3.host")")
echo "  s3 on $N3 ($(cat "$SHARED/state/s3.addr"))"
efs_assert_distinct_servers
echo "$N1 $N2 $N3" > "$PROF_ROOT/server_nodes.txt"
cp -f "$SHARED/state"/s*.addr "$PROF_ROOT/" 2>/dev/null || true

EXCLUDE="${N1},${N2},${N3}"
echo "mkfs export=$EXPORT_NAME ..."
MKFS=$(EXPORT_NAME="$EXPORT_NAME" sbatch --parsable -p "$PARTITION" --time=00:10:00 \
    --exclude="$EXCLUDE" --export=ALL,EXPORT_NAME="$EXPORT_NAME" \
    slurm-jobs/mkfs-ib.sh)
echo "  mkfs job $MKFS"
while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do
    sleep 5
done
sacct -j "$MKFS" -n -o State,ExitCode | head -1
if ! sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED; then
    echo "mkfs failed"; cat "$SHARED/logs/mkfs-${MKFS}".* 2>/dev/null || true
    scancel "$JOB1" "$JOB2" "$JOB3" 2>/dev/null || true
    exit 1
fi

echo "Submitting $NUM_CLIENTS stream clients (exclude=$EXCLUDE); GO after all RUNNING..."
CLIENT_JOBS=()
CLIENT_NODES=()
for cid in $(seq 1 "$NUM_CLIENTS"); do
    excl="$EXCLUDE"
    for prev in "${CLIENT_NODES[@]:-}"; do
        [ -n "$prev" ] && excl="${excl},${prev}"
    done
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
            --exclude="$excl" \
            --export=ALL,ROUND="$ROUND",CLIENT_ID="$cid",FILE_GIB="$FILE_GIB",FILES_PER_CLIENT="$FILES_PER_CLIENT",EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF_ROOT" \
            slurm-jobs/client-stream-ib.sh)
    CLIENT_JOBS+=("$job")
    echo "  client $cid job $job (exclude=$excl)"
    # Wait until scheduled so we can exclude its node for the next client
    for _ in $(seq 1 360); do
        state=$(squeue -h -j "$job" -o '%T' 2>/dev/null || true)
        node=$(squeue -h -j "$job" -o '%N' 2>/dev/null || true)
        if [ "$state" = "RUNNING" ] && [ -n "$node" ]; then
            CLIENT_NODES+=("$node")
            echo "    client $cid on $node"
            break
        fi
        squeue -h -j "$job" 2>/dev/null | grep -q . || break
        sleep 5
    done
done
echo "${CLIENT_JOBS[*]}" > "$PROF_ROOT/client_jobs.txt"
echo "client_nodes: ${CLIENT_NODES[*]:-}" | tee "$PROF_ROOT/client_nodes.txt"

# Start all writers together once every client job is up.
date -Is > "$PROF_ROOT/GO"
echo "GO $(cat "$PROF_ROOT/GO")"

echo "Waiting for client jobs..."
for job in "${CLIENT_JOBS[@]}"; do
    while squeue -h -j "$job" 2>/dev/null | grep -q .; do
        sleep 60
        echo "  $(date +%H:%M:%S) $(squeue -h -j "$job" -o '%j %T %M %N' 2>/dev/null || true)"
    done
    echo "  job $job finished"
done

echo "Tearing down servers..."
for job in "$JOB1" "$JOB2" "$JOB3"; do
    scancel "$job" 2>/dev/null || true
done
sleep 5
squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-str-cli 2>/dev/null | xargs -r scancel || true

# Aggregate
echo "=== aggregate results (round $ROUND) ==="
total_bytes=0
ok=1
earliest=""
latest=""
{
    echo "round=$ROUND file_gib=$FILE_GIB files_per_client=$FILES_PER_CLIENT num_clients=$NUM_CLIENTS"
    echo "servers=$(cat "$PROF_ROOT/server_nodes.txt" 2>/dev/null || true)"
    echo "export=$EXPORT_NAME"
} > "$PROF_ROOT/SUMMARY.txt"

for d in "$PROF_ROOT"/client*-*; do
    [ -d "$d" ] || continue
    echo "--- $(basename "$d") ---"
    cat "$d/IB.txt" 2>/dev/null || true
    if [ -f "$d/SUMMARY.txt" ]; then
        cat "$d/SUMMARY.txt"
        read -r wall bytes mibs gibs cok < "$d/SUMMARY.txt" || true
        if [ "${cok:-0}" != "1" ]; then
            ok=0
        else
            total_bytes=$((total_bytes + bytes))
        fi
    else
        ok=0
        echo "MISSING SUMMARY"
    fi
    if [ -f "$d/wall_start.txt" ]; then
        s=$(cat "$d/wall_start.txt")
        if [ -z "$earliest" ] || awk -v a="$s" -v b="$earliest" 'BEGIN{exit !(a<b)}'; then
            earliest=$s
        fi
    fi
    if [ -f "$d/wall_end.txt" ]; then
        e=$(cat "$d/wall_end.txt")
        if [ -z "$latest" ] || awk -v a="$e" -v b="$latest" 'BEGIN{exit !(a>b)}'; then
            latest=$e
        fi
    fi
    echo "perf top:"
    grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$d/client.report.txt" 2>/dev/null | head -12 || true
done

if [ -n "$earliest" ] && [ -n "$latest" ]; then
    wall=$(awk -v s="$earliest" -v e="$latest" 'BEGIN{printf "%.3f", e-s}')
else
    wall=0
fi
[ "$(awk -v t="$wall" 'BEGIN{print (t<0.001)?1:0}')" = "1" ] && wall=0.001
agg_mibs=$(awk -v b="$total_bytes" -v t="$wall" 'BEGIN{printf "%.2f", (b/1024/1024)/t}')
agg_gibs=$(awk -v b="$total_bytes" -v t="$wall" 'BEGIN{printf "%.3f", (b/1024/1024/1024)/t}')
echo "aggregate_wall_sec=$wall aggregate_bytes=$total_bytes aggregate_MiB_s=$agg_mibs aggregate_GiB_s=$agg_gibs ok=$ok"
{
    echo "aggregate_wall_sec=$wall"
    echo "aggregate_bytes=$total_bytes"
    echo "aggregate_MiB_s=$agg_mibs"
    echo "aggregate_GiB_s=$agg_gibs"
    echo "ok=$ok"
} >> "$PROF_ROOT/SUMMARY.txt"

ln -sfn "$PROF_ROOT" "$SHARED/profile/stream-4x100-ib-latest"
echo "PROF_ROOT=$PROF_ROOT"
echo "Done."
[ "$ok" = "1" ]
