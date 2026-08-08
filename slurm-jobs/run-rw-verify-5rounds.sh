#!/bin/bash
# 5 rounds: 3 servers (1 storage each) × 2 concurrent clients.
# Each round: write urandom → sha256 check → timed readback → sha256 check.
# Client 1 runs under --perf; server 1 under --perf for hotspot capture.
#
# Usage (from login node):
#   FILE_GIB=1 FILES_PER_CLIENT=1 PARTITION=mit_normal \
#     ./slurm-jobs/run-rw-verify-5rounds.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
FILE_GIB="${FILE_GIB:-1}"
FILES_PER_CLIENT="${FILES_PER_CLIENT:-1}"
NUM_CLIENTS=2
NUM_SERVERS=3
ROUNDS="${ROUNDS:-5}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-00:45:00}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

CAMPAIGN="$SHARED/profile/rw-verify-5r-$(date +%Y%m%d-%H%M%S)"
LEDGER="$CAMPAIGN/ledger.tsv"
mkdir -p "$CAMPAIGN" "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
printf 'round\tintegrity\twrite_GiB\twrite_s\twrite_GiB_s\tread_GiB\tread_s\tread_GiB_s\tservers\tclients\tprof\n' \
    > "$LEDGER"

echo "Building..."
BUILD_JOB=$(sbatch --parsable --wait -p "$PARTITION" --time=00:10:00 slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

echo "Campaign: $CAMPAIGN"
echo "Config: ${NUM_SERVERS}srv × ${NUM_CLIENTS}cli × ${FILES_PER_CLIENT}file × ${FILE_GIB}GiB × ${ROUNDS} rounds"

for ROUND in $(seq 1 "$ROUNDS"); do
    echo ""
    echo "############################################"
    echo "### ROUND $ROUND / $ROUNDS  $(date -Is)"
    echo "############################################"

    EXPORT_NAME="rwv-r${ROUND}"
    PROF_ROOT="$CAMPAIGN/round-${ROUND}"
    mkdir -p "$PROF_ROOT"
    rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
    rm -f "$PROF_ROOT/GO"

    PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-rw-cli,efs-mkfs 2>/dev/null || true)
    if [ -n "$PREV" ]; then
        echo "$PREV" | xargs -r scancel || true
        sleep 3
    fi

    SERVER_JOBS=()
    SERVER_NODES=()
    EXCLUDE=""
    for sid in $(seq 1 "$NUM_SERVERS"); do
        excl_args=()
        [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
        # Server 1 gets --perf for hotspot capture this round
        extra=""
        if [ "$sid" = "1" ]; then
            extra="--perf"
        fi
        job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
            --cpus-per-task=4 --mem=8G "${excl_args[@]}" \
            --export=ALL,EFS_DIO=off,EFS_EXTRA_ARGS="$extra" \
            "slurm-jobs/server${sid}.sh")
        SERVER_JOBS+=("$job")
        efs_wait_addr "$sid" 600
        node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
        SERVER_NODES+=("$node")
        if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
        echo "  s${sid} job=$job node=$node"
    done
    efs_assert_distinct_servers "$NUM_SERVERS"
    echo "${SERVER_NODES[*]}" | tee "$PROF_ROOT/server_nodes.txt"

    for job in "${SERVER_JOBS[@]}"; do
        log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
        for _ in $(seq 1 90); do
            grep -q "listening on" "$log" 2>/dev/null && break
            sleep 1
        done
        grep -E 'listening on|Joined|writers=' "$log" | tail -2 || true
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
        [ "$ok" = 1 ] || { echo "ERROR: $addr unreachable"; exit 1; }
    done

    MKFS_OK=0
    for attempt in 1 2 3; do
        MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 \
            --exclude="$EXCLUDE" --export=ALL,EXPORT_NAME="$EXPORT_NAME" \
            slurm-jobs/mkfs-ib.sh)
        echo "mkfs attempt $attempt job $MKFS"
        while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 5; done
        if sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED; then
            MKFS_OK=1
            break
        fi
        sleep 2
    done
    [ "$MKFS_OK" = 1 ] || { echo "mkfs failed round $ROUND"; exit 1; }

    CLIENT_JOBS=()
    CLIENT_NODES=()
    for cid in $(seq 1 "$NUM_CLIENTS"); do
        excl="$EXCLUDE"
        for prev in "${CLIENT_NODES[@]:-}"; do
            [ -n "$prev" ] && excl="${excl},${prev}"
        done
        # Client 1: perf for hotpath
        fuse_perf=0
        [ "$cid" = "1" ] && fuse_perf=1
        job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
            --cpus-per-task=8 --mem=16G --exclude="$excl" \
            --export=ALL,ROUND="$ROUND",CLIENT_ID="$cid",FILE_GIB="$FILE_GIB",FILES_PER_CLIENT="$FILES_PER_CLIENT",EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF_ROOT",NUM_SERVERS="$NUM_SERVERS",EFS_META_BATCH_OPS=65536,EFS_FUSE_PERF="$fuse_perf" \
            slurm-jobs/client-rw-verify-ib.sh)
        CLIENT_JOBS+=("$job")
        echo "  client $cid job=$job perf=$fuse_perf"
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
    echo "GO round $ROUND $(date -Is)"

    echo "Waiting for clients..."
    for job in "${CLIENT_JOBS[@]}"; do
        while squeue -h -j "$job" 2>/dev/null | grep -q .; do
            sleep 30
            echo "  $(date +%H:%M:%S) $(squeue -h -j "$job" -o '%i %T %M %N' 2>/dev/null || true)"
        done
    done

    for job in "${SERVER_JOBS[@]}"; do
        scancel "$job" 2>/dev/null || true
    done
    sleep 2

    # Aggregate metrics from both clients
    integ=PASS
    w_bytes=0
    r_bytes=0
    w_wall=0
    r_wall=0
    while IFS= read -r -d '' f; do
        # metrics.txt: W_WALL w_bytes w_gibs R_WALL r_bytes r_gibs ok
        read -r ww wb wg rw rb rg ok < "$f" || true
        w_bytes=$((w_bytes + wb))
        r_bytes=$((r_bytes + rb))
        awk -v e="$ww" -v m="$w_wall" 'BEGIN{if(e+0>m+0) print e; else print m}' >"$PROF_ROOT/.mw"
        w_wall=$(cat "$PROF_ROOT/.mw")
        awk -v e="$rw" -v m="$r_wall" 'BEGIN{if(e+0>m+0) print e; else print m}' >"$PROF_ROOT/.mr"
        r_wall=$(cat "$PROF_ROOT/.mr")
        [ "$ok" = "1" ] || integ=FAIL
    done < <(find "$PROF_ROOT" -name metrics.txt -print0 2>/dev/null)

    # Also require every integrity-*.txt OK
    while IFS= read -r -d '' f; do
        grep -q '^OK ' "$f" || integ=FAIL
    done < <(find "$PROF_ROOT" -name 'integrity-*.txt' -print0 2>/dev/null)

    w_gib=$(awk -v b="$w_bytes" 'BEGIN{printf "%.3f", b/1024/1024/1024}')
    r_gib=$(awk -v b="$r_bytes" 'BEGIN{printf "%.3f", b/1024/1024/1024}')
    w_gps=$(awk -v g="$w_gib" -v t="$w_wall" 'BEGIN{if(t+0>0) printf "%.3f", g/t; else print 0}')
    r_gps=$(awk -v g="$r_gib" -v t="$r_wall" 'BEGIN{if(t+0>0) printf "%.3f", g/t; else print 0}')

    {
        echo "round=$ROUND integrity=$integ"
        echo "write: ${w_gib} GiB in ${w_wall}s → ${w_gps} GiB/s aggregate"
        echo "read:  ${r_gib} GiB in ${r_wall}s → ${r_gps} GiB/s aggregate"
        echo "servers=${SERVER_NODES[*]} clients=${CLIENT_NODES[*]}"
        echo "--- integrity files ---"
        find "$PROF_ROOT" -name 'integrity-*.txt' -exec echo {} \; -exec cat {} \;
        echo "--- client hotpath (cli1) ---"
        find "$PROF_ROOT" -name 'client.hotpath.txt' -exec cat {} \; 2>/dev/null | head -40 || true
        echo "--- client top symbols ---"
        find "$PROF_ROOT" -path '*/client1-*/client.report.txt' -exec \
            grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' {} \; 2>/dev/null | head -25 || true
        echo "--- server1 listen banner ---"
        sj=${SERVER_JOBS[0]}
        slog=$(ls -t "$SHARED/logs"/s1-"${sj}".out 2>/dev/null | head -1 || true)
        grep -E 'listening on|writers=' "$slog" 2>/dev/null | tail -5 || true
        if [ -f "$SHARED/profile" ]; then :; fi
        # server perf if copied — efsd writes under job scratch; may be gone.
        # Look for server.perf in shared logs? server cleanup removes scratch.
        # Capture: scancel may kill before copy — acceptable; client hotpath is primary.
    } | tee "$PROF_ROOT/SUMMARY.txt"

    srv_slash=$(IFS=/; echo "${SERVER_NODES[*]}")
    cli_slash=$(IFS=/; echo "${CLIENT_NODES[*]}")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$ROUND" "$integ" "$w_gib" "$w_wall" "$w_gps" "$r_gib" "$r_wall" "$r_gps" \
        "$srv_slash" "$cli_slash" "$PROF_ROOT" >> "$LEDGER"

    echo "=== ledger so far ==="
    column -t -s $'\t' "$LEDGER" 2>/dev/null || cat "$LEDGER"

    if [ "$integ" != "PASS" ]; then
        echo "FATAL: integrity FAIL on round $ROUND — stopping campaign"
        exit 1
    fi
done

{
    echo "===== CAMPAIGN COMPLETE $(date -Is) ====="
    echo "CAMPAIGN=$CAMPAIGN"
    column -t -s $'\t' "$LEDGER" 2>/dev/null || cat "$LEDGER"
    echo ""
    echo "=== Hotpath synthesis (cli1 reports across rounds) ==="
    for r in $(seq 1 "$ROUNDS"); do
        echo "--- round $r ---"
        find "$CAMPAIGN/round-$r" -name 'client.hotpath.txt' -exec cat {} \; 2>/dev/null | head -20 || true
        find "$CAMPAIGN/round-$r" -path '*/client1-*/client.report.txt' 2>/dev/null | while read -r rep; do
            grep -E '^\s+[0-9]+\.[0-9]+%' "$rep" | head -12 || true
        done
    done
} | tee "$CAMPAIGN/FINAL.txt"

rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node 2>/dev/null || true
echo "DONE $CAMPAIGN"
