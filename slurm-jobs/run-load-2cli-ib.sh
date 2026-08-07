#!/bin/bash
# 2-client IB load harness. Measures aggregate logical write and estimates
# client egress (×1.5 for 2+1 EC fragment traffic).
#
# Usage:
#   ITER=1 FILE_GIB=2 FILES_PER_CLIENT=2 PARTITION=mit_quicktest \
#     ./slurm-jobs/run-load-2cli-ib.sh
#   NUM_SERVERS=4 NOTE='4srv+8x2GiB' FILE_GIB=2 FILES_PER_CLIENT=8 \
#     ./slurm-jobs/run-load-2cli-ib.sh
#
# Ledger: /orcd/scratch/.../efs/profile/load-ledger.tsv

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ITER="${ITER:-1}"
FILE_GIB="${FILE_GIB:-2}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-2}"
NUM_CLIENTS="${NUM_CLIENTS:-2}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-00:30:00}"
CLIENT_CPUS="${CLIENT_CPUS:-8}"
CLIENT_MEM="${CLIENT_MEM:-8G}"
EXPORT_NAME="load2cli-i${ITER}"
NOTE="${NOTE:-}"
DO_READ="${DO_READ:-1}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

if [ "$NUM_SERVERS" -lt 3 ] || [ "$NUM_SERVERS" -gt 4 ]; then
    echo "ERROR: NUM_SERVERS must be 3 or 4 (got $NUM_SERVERS)"
    exit 1
fi

LEDGER="$SHARED/profile/load-ledger.tsv"
PROF_ROOT="$SHARED/profile/load-2cli-i${ITER}-$(date +%H%M%S)"
mkdir -p "$PROF_ROOT" "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
if [ ! -f "$LEDGER" ]; then
    printf 'iter\tnote\tlogical_GiB\twall_s\tlogical_GiB_s\tegress_GiB_s\tnodes\n' > "$LEDGER"
fi

echo "Building..."
BUILD_JOB=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
rm -f "$PROF_ROOT/GO"

PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-s4,efs-str-cli,efs-mkfs 2>/dev/null || true)
if [ -n "$PREV" ]; then
    echo "$PREV" | xargs -r scancel || true
    sleep 2
fi

echo "=== iter=$ITER note='$NOTE' ${NUM_SERVERS}srv ${NUM_CLIENTS}cli × ${FILES_PER_CLIENT} × ${FILE_GIB}GiB ==="

SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in $(seq 1 "$NUM_SERVERS"); do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
        --cpus-per-task=4 --mem=8G "${excl_args[@]}" \
        --export=ALL,EFS_DIO=on "slurm-jobs/server${sid}.sh")
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    SERVER_NODES+=("$node")
    if [ -z "$EXCLUDE" ]; then
        EXCLUDE="$node"
    else
        EXCLUDE="${EXCLUDE},${node}"
    fi
    echo "  s${sid} job $job on $node"
done

efs_assert_distinct_servers "$NUM_SERVERS"
echo "${SERVER_NODES[*]}" | tee "$PROF_ROOT/server_nodes.txt"

# Wait for listen/join
for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 60); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
    grep -E 'listening on|Joined|writers=' "$log" | tail -3 || true
done
sleep 5

# Confirm servers still alive over IB before mkfs.
for i in $(seq 1 "$NUM_SERVERS"); do
    addr=$(cat "$SHARED/state/s${i}.addr")
    host=${addr%:*}
    port=${addr#*:}
    ok=0
    for _ in $(seq 1 30); do
        if timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; then ok=1; break; fi
        sleep 1
    done
    [ "$ok" = 1 ] || { echo "ERROR: $addr not reachable before mkfs"; exit 1; }
    sj=${SERVER_JOBS[$((i - 1))]}
    squeue -h -j "$sj" >/dev/null || { echo "ERROR: server job $sj gone before mkfs"; exit 1; }
done

MKFS_OK=0
for attempt in 1 2 3; do
    MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 \
        --exclude="$EXCLUDE" --export=ALL,EXPORT_NAME="$EXPORT_NAME" \
        slurm-jobs/mkfs-ib.sh)
    echo "mkfs attempt $attempt job $MKFS (exclude=$EXCLUDE)"
    while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 5; done
    if sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED; then
        MKFS_OK=1
        break
    fi
    echo "mkfs attempt $attempt failed; retrying..."
    cat "$SHARED/logs/mkfs-${MKFS}.out" 2>/dev/null || true
    sleep 2
done
[ "$MKFS_OK" = 1 ] || { echo "mkfs failed"; exit 1; }

CLIENT_JOBS=()
CLIENT_NODES=()
for cid in $(seq 1 "$NUM_CLIENTS"); do
    excl="$EXCLUDE"
    for prev in "${CLIENT_NODES[@]:-}"; do
        [ -n "$prev" ] && excl="${excl},${prev}"
    done
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
        --cpus-per-task="$CLIENT_CPUS" --mem="$CLIENT_MEM" --exclude="$excl" \
        --export=ALL,ROUND="$ITER",CLIENT_ID="$cid",FILE_GIB="$FILE_GIB",FILES_PER_CLIENT="$FILES_PER_CLIENT",EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF_ROOT",NUM_SERVERS="$NUM_SERVERS",EFS_META_BATCH_OPS=65536,DO_READ="$DO_READ",EFS_NUMA_AFFINITY="${EFS_NUMA_AFFINITY:-}" \
        slurm-jobs/client-stream-ib.sh)
    CLIENT_JOBS+=("$job")
    echo "  client $cid job $job"
    for _ in $(seq 1 180); do
        state=$(squeue -h -j "$job" -o '%T' 2>/dev/null || true)
        node=$(squeue -h -j "$job" -o '%N' 2>/dev/null || true)
        if [ "$state" = "RUNNING" ] && [ -n "$node" ]; then
            CLIENT_NODES+=("$node")
            echo "    on $node"
            break
        fi
        if [ -z "$state" ]; then
            echo "client $cid vanished before RUNNING"
            break
        fi
        sleep 2
    done
done

touch "$PROF_ROOT/GO"
echo "GO $(date -Is)"

echo "Waiting for clients..."
for job in "${CLIENT_JOBS[@]}"; do
    while squeue -h -j "$job" 2>/dev/null | grep -q .; do
        sleep 30
        echo "  $(date +%H:%M:%S) $(squeue -h -j "$job" -o '%T %M %N' 2>/dev/null || true)"
    done
done

for job in "${SERVER_JOBS[@]}"; do
    scancel "$job" 2>/dev/null || true
done
sleep 2

# Aggregate results from writer-*.result files: "elapsed bytes mibs gibs"
total_bytes=0
max_wall=0
while IFS= read -r -d '' f; do
    read -r elapsed bytes mibs gibs < "$f" || true
    total_bytes=$((total_bytes + bytes))
    awk -v e="$elapsed" -v m="$max_wall" 'BEGIN{if(e+0>m+0) print e; else print m}' \
        >"$PROF_ROOT/.maxwall.tmp" 2>/dev/null || echo "$elapsed" >"$PROF_ROOT/.maxwall.tmp"
    max_wall=$(cat "$PROF_ROOT/.maxwall.tmp")
done < <(find "$PROF_ROOT" -name 'writer-*.result' -print0 2>/dev/null)

logical_gib=$(awk -v b="$total_bytes" 'BEGIN{printf "%.3f", b/1024/1024/1024}')
# Overlap wall: use max of per-file walls (parallel writers)
wall=${max_wall:-0}
logical_gps=$(awk -v g="$logical_gib" -v t="$wall" 'BEGIN{if(t+0>0) printf "%.3f", g/t; else print 0}')
# 2+1 EC: each logical byte becomes 1.5 fragment bytes on the wire from client
egress_gps=$(awk -v g="$logical_gps" 'BEGIN{printf "%.3f", g*1.5}')

srv_slash=$(IFS=/; echo "${SERVER_NODES[*]}")
{
    echo "iter=$ITER note=$NOTE num_servers=$NUM_SERVERS"
    echo "servers=${SERVER_NODES[*]} clients=${CLIENT_NODES[*]}"
    echo "logical_GiB=$logical_gib wall_s=$wall logical_GiB_s=$logical_gps egress_GiB_s~$egress_gps"
    find "$PROF_ROOT" -name 'writer-*.result' -exec echo {} \; -exec cat {} \;
} | tee "$PROF_ROOT/SUMMARY.txt"

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$ITER" "$NOTE" "$logical_gib" "$wall" "$logical_gps" "$egress_gps" \
    "s=${srv_slash} c=${CLIENT_NODES[*]}" >> "$LEDGER"

echo "=== LEDGER ==="
column -t -s $'\t' "$LEDGER" 2>/dev/null || cat "$LEDGER"
echo "PROF=$PROF_ROOT"
