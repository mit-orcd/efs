#!/bin/bash
# Start a 3-server IB cluster under 2-client write load, then join a 4th
# server mid-flight. Records membership/used via efs-mgmt status and whether
# s4 absorbs any write traffic (expect: no, without remount/rebalance).
#
# Usage:
#   FILE_GIB=4 FILES_PER_CLIENT=8 PARTITION=mit_normal \
#     ./slurm-jobs/run-load-join4-ib.sh
#
# Results: /orcd/scratch/.../efs/profile/join4-<ts>/

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
FILE_GIB="${FILE_GIB:-4}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-8}"
NUM_CLIENTS="${NUM_CLIENTS:-2}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-01:00:00}"
JOIN_DELAY_S="${JOIN_DELAY_S:-5}"
EXPORT_NAME="join4load"
NOTE="${NOTE:-join4_midwrite}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

PROF_ROOT="$SHARED/profile/join4-$(date +%H%M%S)"
LEDGER="$SHARED/profile/join4-ledger.tsv"
mkdir -p "$PROF_ROOT" "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
if [ ! -f "$LEDGER" ]; then
    printf 'note\tlogical_GiB\twall_s\tlogical_GiB_s\tegress_GiB_s\ts4_used_bytes\tnodes\n' > "$LEDGER"
fi

echo "Building..."
BUILD_JOB=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
rm -f "$PROF_ROOT/GO" "$PROF_ROOT/JOINED"

PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-s4,efs-str-cli,efs-mkfs,efs-join4v 2>/dev/null || true)
if [ -n "$PREV" ]; then
    echo "$PREV" | xargs -r scancel || true
    sleep 2
fi

echo "=== $NOTE: 3srv start, join s4 after ${JOIN_DELAY_S}s under ${NUM_CLIENTS}×${FILES_PER_CLIENT}×${FILE_GIB}GiB ==="

SERVER_JOBS=()
SERVER_NODES=()
EXCLUDE=""
for sid in 1 2 3; do
    excl_args=()
    [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
        --cpus-per-task=4 --mem=8G "${excl_args[@]}" \
        --export=ALL,EFS_DIO=off "slurm-jobs/server${sid}.sh")
    SERVER_JOBS+=("$job")
    efs_wait_addr "$sid" 600
    node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
    SERVER_NODES+=("$node")
    EXCLUDE="${EXCLUDE:+$EXCLUDE,}$node"
    echo "  s${sid} job $job on $node"
done
efs_assert_distinct_servers 3
echo "${SERVER_NODES[*]}" | tee "$PROF_ROOT/server_nodes_pre.txt"

for job in "${SERVER_JOBS[@]}"; do
    log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
    for _ in $(seq 1 60); do
        grep -q "listening on" "$log" 2>/dev/null && break
        sleep 1
    done
    grep -E 'listening on|Joined|writers=' "$log" | tail -3 || true
done
sleep 5

for i in 1 2 3; do
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

MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 \
    --exclude="$EXCLUDE" --export=ALL,EXPORT_NAME="$EXPORT_NAME" \
    slurm-jobs/mkfs-ib.sh)
echo "mkfs job $MKFS"
while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 5; done
sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED || { echo "mkfs failed"; exit 1; }

# Snapshot membership before clients (must run on a compute node — IB addrs).
ST=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,STATUS_LABEL=pre-load,NUM_SERVERS=3 slurm-jobs/status-ib.sh)
while squeue -h -j "$ST" 2>/dev/null | grep -q .; do sleep 5; done
cp -f "$SHARED/logs/status-${ST}.out" "$PROF_ROOT/status-pre.txt" 2>/dev/null || true
cat "$PROF_ROOT/status-pre.txt" || true

CLIENT_JOBS=()
CLIENT_NODES=()
for cid in $(seq 1 "$NUM_CLIENTS"); do
    excl="$EXCLUDE"
    for prev in "${CLIENT_NODES[@]:-}"; do
        [ -n "$prev" ] && excl="${excl},${prev}"
    done
    job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
        --cpus-per-task=8 --mem=8G --exclude="$excl" \
        --export=ALL,ROUND=join4,CLIENT_ID="$cid",FILE_GIB="$FILE_GIB",FILES_PER_CLIENT="$FILES_PER_CLIENT",EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF_ROOT",NUM_SERVERS=3,EFS_META_BATCH_OPS=65536 \
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
        [ -z "$state" ] && break
        sleep 2
    done
done

touch "$PROF_ROOT/GO"
echo "GO $(date -Is)" | tee "$PROF_ROOT/timeline.txt"
sleep "$JOIN_DELAY_S"
echo "JOIN_START $(date -Is) delay=${JOIN_DELAY_S}s" | tee -a "$PROF_ROOT/timeline.txt"

JOB4=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
    --cpus-per-task=4 --mem=8G --exclude="$EXCLUDE" \
    --export=ALL,EFS_DIO=off slurm-jobs/server4.sh)
echo "  s4 job $JOB4 (exclude=$EXCLUDE)"
efs_wait_addr 4 600
N4=$(efs_slurm_node "$(cat "$SHARED/state/s4.host")")
SERVER_JOBS+=("$JOB4")
SERVER_NODES+=("$N4")
EXCLUDE="${EXCLUDE},${N4}"
echo "JOIN_ADDR $(date -Is) s4=$N4 $(cat "$SHARED/state/s4.addr")" | tee -a "$PROF_ROOT/timeline.txt"

# Wait for s4 listen/join banner
log4=$(ls -t "$SHARED/logs"/s4-"${JOB4}".out 2>/dev/null | head -1 || true)
for _ in $(seq 1 90); do
    grep -qE 'listening on|Joined' "$log4" 2>/dev/null && break
    sleep 1
done
grep -E 'listening on|Joined|writers=|Could not|reject|ERROR' "$log4" | tee "$PROF_ROOT/s4-join.log" || true
touch "$PROF_ROOT/JOINED"
echo "JOIN_DONE $(date -Is)" | tee -a "$PROF_ROOT/timeline.txt"

# Poll membership / used while writers still running
: > "$PROF_ROOT/status-during.txt"
for poll in 1 2 3 4; do
    still=0
    for job in "${CLIENT_JOBS[@]}"; do
        squeue -h -j "$job" 2>/dev/null | grep -q . && still=1
    done
    [ "$still" = 0 ] && break
    ST=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
        --export=ALL,STATUS_LABEL="during-p${poll}",NUM_SERVERS=4 slurm-jobs/status-ib.sh)
    while squeue -h -j "$ST" 2>/dev/null | grep -q .; do sleep 5; done
    {
        echo "=== poll $poll $(date -Is) job=$ST ==="
        cat "$SHARED/logs/status-${ST}.out" 2>/dev/null || true
    } | tee -a "$PROF_ROOT/status-during.txt"
    sleep 5
done

echo "Waiting for clients..."
for job in "${CLIENT_JOBS[@]}"; do
    while squeue -h -j "$job" 2>/dev/null | grep -q .; do
        sleep 30
        echo "  $(date +%H:%M:%S) $(squeue -h -j "$job" -o '%T %M %N' 2>/dev/null || true)"
    done
done
echo "CLIENTS_DONE $(date -Is)" | tee -a "$PROF_ROOT/timeline.txt"

ST=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,STATUS_LABEL=post-load,NUM_SERVERS=4 slurm-jobs/status-ib.sh)
while squeue -h -j "$ST" 2>/dev/null | grep -q .; do sleep 5; done
cp -f "$SHARED/logs/status-${ST}.out" "$PROF_ROOT/status-post.txt" 2>/dev/null || true
cat "$PROF_ROOT/status-post.txt" || true

# Aggregate writer results
total_bytes=0
max_wall=0
while IFS= read -r -d '' f; do
    read -r elapsed bytes mibs gibs < "$f" || true
    case "$elapsed" in FAIL|"") continue ;; esac
    total_bytes=$((total_bytes + bytes))
    max_wall=$(awk -v e="$elapsed" -v m="$max_wall" 'BEGIN{print (e+0>m+0)?e:m}')
done < <(find "$PROF_ROOT" -name 'writer-*.result' -print0 2>/dev/null)

logical_gib=$(awk -v b="$total_bytes" 'BEGIN{printf "%.3f", b/1024/1024/1024}')
wall=${max_wall:-0}
logical_gps=$(awk -v g="$logical_gib" -v t="$wall" 'BEGIN{if(t+0>0) printf "%.3f", g/t; else print 0}')
egress_gps=$(awk -v g="$logical_gps" 'BEGIN{printf "%.3f", g*1.5}')

s4_line=$(grep -E 'node 4:' "$PROF_ROOT/status-post.txt" | head -1 || echo "missing")

# Remount verify: discover 4 nodes, try read of an old file + write a new small file
VERIFY=$(sbatch --parsable -p "$PARTITION" --time=00:20:00 \
    --cpus-per-task=2 --mem=4G --exclude="$EXCLUDE" \
    --export=ALL,PROF_ROOT="$PROF_ROOT",EXPORT_NAME="$EXPORT_NAME" \
    slurm-jobs/client-join4-verify.sh)
echo "verify job $VERIFY"
while squeue -h -j "$VERIFY" 2>/dev/null | grep -q .; do sleep 10; done
cat "$SHARED/logs/join4v-${VERIFY}.out" 2>/dev/null | tee "$PROF_ROOT/verify.out" || true

ST=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
    --export=ALL,STATUS_LABEL=post-verify,NUM_SERVERS=4 slurm-jobs/status-ib.sh)
while squeue -h -j "$ST" 2>/dev/null | grep -q .; do sleep 5; done
cp -f "$SHARED/logs/status-${ST}.out" "$PROF_ROOT/status-final.txt" 2>/dev/null || true
cat "$PROF_ROOT/status-final.txt" || true

for job in "${SERVER_JOBS[@]}"; do
    scancel "$job" 2>/dev/null || true
done

srv_slash=$(IFS=/; echo "${SERVER_NODES[*]}")
{
    echo "note=$NOTE"
    echo "servers=${SERVER_NODES[*]} clients=${CLIENT_NODES[*]}"
    echo "logical_GiB=$logical_gib wall_s=$wall logical_GiB_s=$logical_gps egress_GiB_s~$egress_gps"
    echo "s4_status_line=$s4_line"
    echo "join_delay_s=$JOIN_DELAY_S"
} | tee "$PROF_ROOT/SUMMARY.txt"

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$NOTE" "$logical_gib" "$wall" "$logical_gps" "$egress_gps" \
    "$s4_line" "s=${srv_slash} c=${CLIENT_NODES[*]}" >> "$LEDGER"

echo "=== JOIN4 DONE PROF=$PROF_ROOT ==="
column -t -s $'\t' "$LEDGER" 2>/dev/null || cat "$LEDGER"
