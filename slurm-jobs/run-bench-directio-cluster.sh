#!/bin/bash
# Multi-node buffered vs direct-io bench over InfiniBand.
# 3 servers on distinct nodes + 1 client excluded from those nodes.
# Default writers=8 (efsd default). buffered → EFS_DIO=off; direct → EFS_DIO=on.
#
# Usage: PARTITION=mit_quicktest ./slurm-jobs/run-bench-directio-cluster.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PARTITION="${PARTITION:-mit_quicktest}"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$SHARED/profile/dio-cluster-${STAMP}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

echo "Building efs binaries via Slurm..."
BUILD_JOB=$(sbatch --parsable --wait -p "$PARTITION" slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

mkdir -p "$OUT" "$SHARED/state" "$SHARED/logs"
echo "params: multi-node IB cluster, writers=8 (default)" | tee "$OUT/results.txt"
echo "partition=$PARTITION out=$OUT" | tee -a "$OUT/results.txt"

run_mode() {
    local mode=$1
    # EFS_DIO=off|on — avoid putting leading "--..." into sbatch --export values.
    local dio_flag=$2
    echo
    echo "======== mode=$mode dio=$dio_flag ========"

    rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
    PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-dio-cli 2>/dev/null || true)
    if [ -n "$PREV" ]; then
        echo "Cancelling previous jobs: $PREV"
        echo "$PREV" | xargs -r scancel || true
        sleep 3
    fi

    echo "Submitting IB servers on distinct nodes (partition=$PARTITION)..."
    JOB1=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
        --cpus-per-task=4 --mem=4G \
        --export=ALL,EFS_DIO="$dio_flag" slurm-jobs/server1.sh)
    echo "  s1 job $JOB1"
    efs_wait_addr 1 600
    N1=$(efs_slurm_node "$(cat "$SHARED/state/s1.host")")
    echo "  s1 on $N1 ($(cat "$SHARED/state/s1.addr"))"

    JOB2=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
        --cpus-per-task=4 --mem=4G --exclude="$N1" \
        --export=ALL,EFS_DIO="$dio_flag" slurm-jobs/server2.sh)
    echo "  s2 job $JOB2 (exclude=$N1)"
    efs_wait_addr 2 600
    N2=$(efs_slurm_node "$(cat "$SHARED/state/s2.host")")
    echo "  s2 on $N2 ($(cat "$SHARED/state/s2.addr"))"

    JOB3=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
        --cpus-per-task=4 --mem=4G --exclude="${N1},${N2}" \
        --export=ALL,EFS_DIO="$dio_flag" slurm-jobs/server3.sh)
    echo "  s3 job $JOB3 (exclude=${N1},${N2})"
    efs_wait_addr 3 600
    N3=$(efs_slurm_node "$(cat "$SHARED/state/s3.host")")
    echo "  s3 on $N3 ($(cat "$SHARED/state/s3.addr"))"

    efs_assert_distinct_servers
    for i in 1 2 3; do
        addr=$(cat "$SHARED/state/s${i}.addr")
        case "$addr" in
            *.ib:*) ;;
            *)
                echo "ERROR: s${i}.addr is not an IB hostname: $addr"
                squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-dio-cli 2>/dev/null | xargs -r scancel || true
                return 1
                ;;
        esac
    done

    # .addr is written before efsd finishes join — wait for listen banners and
    # a full 3-node membership before starting the client.
    wait_log() {
        local job=$1
        local pat=$2
        local label=$3
        local log waited=0
        log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
        [ -n "$log" ] || { echo "ERROR: no log for job $job"; return 1; }
        while ! grep -q "$pat" "$log" 2>/dev/null; do
            sleep 2
            waited=$((waited + 2))
            if [ "$waited" -ge 120 ]; then
                echo "ERROR: timeout waiting for '$pat' in $label ($log)"
                tail -20 "$log" || true
                return 1
            fi
        done
        grep -E 'listening on|Joined cluster|direct_io=|writers=' "$log" | tail -5 || true
    }
    wait_log "$JOB1" "listening on" "s1" || return 1
    wait_log "$JOB2" "listening on" "s2" || return 1
    wait_log "$JOB3" "listening on" "s3" || return 1
    # Brief optional wait for join banners (non-fatal if buffered/missing).
    for job_pat in "$JOB2:Joined cluster" "$JOB3:Joined cluster with 3 nodes"; do
        job=${job_pat%%:*}
        pat=${job_pat#*:}
        log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
        for _ in $(seq 1 15); do
            grep -q "$pat" "$log" 2>/dev/null && break
            sleep 1
        done
        grep -E 'Joined cluster|listening on|direct_io=|writers=' "$log" 2>/dev/null | tail -5 || true
    done
    # Give HELLO gossip a moment so LIST_NODES on s1 includes everyone.
    sleep 8

    EXCLUDE_SERVERS="${N1},${N2},${N3}"
    echo "Submitting client (exclude=$EXCLUDE_SERVERS) MODE=$mode ..."
    CLIENT=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
        --exclude="$EXCLUDE_SERVERS" \
        --export=ALL,MODE="$mode",OUT_DIR="$OUT" \
        slurm-jobs/client-bench-directio.sh)
    echo "Client job: $CLIENT"

    echo "Waiting for client job $CLIENT ..."
    while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do
        sleep 60
        echo "  $(date +%H:%M:%S) still running; $(squeue -h -j "$CLIENT" -o '%T %M %N' 2>/dev/null || true)"
    done

    echo "--- client log ---"
    tail -40 "$SHARED/logs/dio-cli-${CLIENT}.out" 2>/dev/null || true
    if [ -f "$SHARED/logs/dio-cli-${CLIENT}.err" ]; then
        echo "--- client err ---"
        tail -20 "$SHARED/logs/dio-cli-${CLIENT}.err" 2>/dev/null || true
    fi

    echo "Tearing down servers..."
    for job in "$JOB1" "$JOB2" "$JOB3"; do
        scancel "$job" 2>/dev/null || true
    done
    sleep 3
    squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-dio-cli 2>/dev/null | xargs -r scancel || true

    if ! grep -q "MODE=$mode DONE" "$SHARED/logs/dio-cli-${CLIENT}.out" 2>/dev/null; then
        echo "ERROR: mode=$mode client did not finish cleanly"
        return 1
    fi
}

run_mode buffered off
run_mode direct on

echo
echo "=== CLUSTER SUMMARY ==="
cat "$OUT/results.txt"
echo "Topology:"
cat "$OUT"/*-topo.txt 2>/dev/null || true
echo "Results: $OUT/results.txt"
echo "$OUT" > "$SHARED/profile/dio-cluster-latest.txt"
