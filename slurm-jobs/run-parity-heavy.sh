#!/bin/bash
# 3-server IB cluster + 2 parallel clients:
#   20 × 1 GiB big files + 1000 × 1 MiB small files in 20 subdirs (disjoint ownership).
# Iterate up to MAX times; stop on first full PASS.
#
# Usage: ./slurm-jobs/run-parity-heavy.sh [MAX]
set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
MAX="${1:-5}"
PARTITION="${PARTITION:-mit_normal}"
JOB_TIME="${JOB_TIME:-03:00:00}"
NUM_SERVERS=3
NUM_CLIENTS=2
EXPORT_NAME_BASE=parityHeavy
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

mkdir -p "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
chmod +x "$REPO/slurm-jobs/client-parity-heavy.sh"

cleanup_cluster() {
    set +e
    local jobs=("$@")
    for j in "${jobs[@]:-}"; do
        [ -n "$j" ] && scancel "$j" 2>/dev/null || true
    done
    sleep 2
}

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "=== clean rebuild ==="
    sbatch --wait -p "$PARTITION" --time=00:15:00 slurm-jobs/build.sh
else
    echo "=== SKIP_BUILD=1 (using existing binaries) ==="
fi

for iter in $(seq 1 "$MAX"); do
    echo ""
    echo "======== PARITY-HEAVY ITER $iter / $MAX ========"
    EXPORT_NAME="${EXPORT_NAME_BASE}-i${iter}"
    PROF_ROOT="$SHARED/profile/parity-heavy-i${iter}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$PROF_ROOT"
    rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
    rm -f "$PROF_ROOT/GO"

    # Cancel any leftover harness jobs from prior iters
    PREV=$(squeue -u erbmi1 -h -o '%i %j' 2>/dev/null | awk '/efs-s[1-4]|efs-par-cli|efs-mkfs/{print $1}' || true)
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
        job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
            --cpus-per-task=4 --mem=16G "${excl_args[@]}" \
            --export=ALL,EFS_DIO=off "slurm-jobs/server${sid}.sh")
        SERVER_JOBS+=("$job")
        efs_wait_addr "$sid" 600
        # Prefer sN.node (short hostname) — sN.host may be numeric IB IP.
        if [ -s "$SHARED/state/s${sid}.node" ]; then
            node=$(cat "$SHARED/state/s${sid}.node")
        else
            node=$(efs_slurm_node "$(cat "$SHARED/state/s${sid}.host")")
        fi
        SERVER_NODES+=("$node")
        if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
        echo "  s${sid} job=$job node=$node"
    done
    # Distinctness via short hostnames in sN.node
    for i in $(seq 0 $((NUM_SERVERS - 1))); do
        for j in $(seq $((i + 1)) $((NUM_SERVERS - 1))); do
            if [ "${SERVER_NODES[$i]}" = "${SERVER_NODES[$j]}" ]; then
                echo "ERROR: servers must run on distinct nodes" >&2
                cleanup_cluster "${SERVER_JOBS[@]}"
                exit 1
            fi
        done
    done
    echo "server nodes: ${SERVER_NODES[*]}"

    for job in "${SERVER_JOBS[@]}"; do
        log=$(ls -t "$SHARED/logs"/s*-"${job}".out 2>/dev/null | head -1 || true)
        ready=0
        for _ in $(seq 1 90); do
            if grep -q "listening on" "$log" 2>/dev/null; then ready=1; break; fi
            if ! squeue -h -j "$job" 2>/dev/null | grep -q .; then
                echo "ERROR: server job $job exited before listen"
                tail -40 "$log" || true
                break
            fi
            sleep 2
        done
        [ "$ready" = 1 ] || { echo "ERROR: no listen in $log"; cleanup_cluster "${SERVER_JOBS[@]}"; continue 2; }
        grep -E 'listening on|Joined|writers=' "$log" | tail -3 || true
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
        [ "$ok" = 1 ] || { echo "ERROR: $addr not reachable"; cleanup_cluster "${SERVER_JOBS[@]}"; exit 1; }
    done

    MKFS_OK=0
    for attempt in 1 2 3; do
        MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 \
            --exclude="$EXCLUDE" --export=ALL,EXPORT_NAME="$EXPORT_NAME" \
            slurm-jobs/mkfs-ib.sh)
        echo "mkfs attempt $attempt job=$MKFS"
        while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 60; done
        if sacct -j "$MKFS" -n -o State | head -1 | grep -q COMPLETED; then
            MKFS_OK=1
            break
        fi
        cat "$SHARED/logs/mkfs-${MKFS}.out" 2>/dev/null || true
    done
    if [ "$MKFS_OK" != 1 ]; then
        echo "PARITY_HEAVY_FAIL iter=$iter reason=mkfs"
        cleanup_cluster "${SERVER_JOBS[@]}"
        continue
    fi

    CLIENT_JOBS=()
    CLIENT_NODES=()
    for cid in $(seq 1 "$NUM_CLIENTS"); do
        excl="$EXCLUDE"
        for prev in "${CLIENT_NODES[@]:-}"; do
            [ -n "$prev" ] && excl="${excl},${prev}"
        done
        job=$(sbatch --parsable -p "$PARTITION" --time="$JOB_TIME" \
            --cpus-per-task=8 --mem=16G --exclude="$excl" \
            --export=ALL,CLIENT_ID="$cid",EXPORT_NAME="$EXPORT_NAME",PROF_ROOT="$PROF_ROOT",NUM_SERVERS="$NUM_SERVERS",EFS_META_BATCH_OPS=65536 \
            slurm-jobs/client-parity-heavy.sh)
        CLIENT_JOBS+=("$job")
        echo "  client $cid job=$job"
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
    echo "GO $(date -Is) PROF=$PROF_ROOT"
    echo "workload: 20×1GiB big + 1000×1MiB small/20dirs from 2 clients (disjoint)"

    echo "Waiting for clients (poll ≤1/min)..."
    for job in "${CLIENT_JOBS[@]}"; do
        while squeue -h -j "$job" 2>/dev/null | grep -q .; do
            sleep 60
            echo "  $(date +%H:%M:%S) $(squeue -h -j "$job" -o '%j %T %M %N' 2>/dev/null || true)"
        done
    done

    ALL_OK=1
    for cid in 1 2; do
        res=$(find "$PROF_ROOT" -path "*/client${cid}-*/RESULT" 2>/dev/null | head -1 || true)
        if [ -n "$res" ] && grep -q CLIENT_HEAVY_OK "$res"; then
            echo "client $cid OK ($res)"
            cat "$res"
        else
            echo "client $cid FAIL"
            ALL_OK=0
            find "$PROF_ROOT" -path "*/client${cid}-*" -name '*.out' -o -path "*/client${cid}-*/RESULT" 2>/dev/null | head -20
            # show client slurm logs
            for job in "${CLIENT_JOBS[@]}"; do
                echo "--- par-cli-${job}.out tail ---"
                tail -60 "$SHARED/logs/par-cli-${job}.out" 2>/dev/null || true
                echo "--- par-cli-${job}.err tail ---"
                tail -40 "$SHARED/logs/par-cli-${job}.err" 2>/dev/null || true
            done
        fi
    done

    cleanup_cluster "${SERVER_JOBS[@]}" "${CLIENT_JOBS[@]}"

    {
        echo "iter=$iter all_ok=$ALL_OK"
        echo "servers=${SERVER_NODES[*]}"
        echo "clients=${CLIENT_NODES[*]}"
        echo "prof=$PROF_ROOT"
    } | tee "$PROF_ROOT/SUMMARY.txt"

    if [ "$ALL_OK" = 1 ]; then
        echo "PARITY_HEAVY_PASS iter=$iter"
        echo "PARITY_HEAVY_PASS" >"$PROF_ROOT/SUITE_RESULT"
        exit 0
    fi

    echo "PARITY_HEAVY_FAIL iter=$iter — inspect $PROF_ROOT"
    echo "PARITY_HEAVY_FAIL" >"$PROF_ROOT/SUITE_RESULT"
    # Leave room for agent to fix code between iters; continue automatically.
    # Next iter rebuilds if agent patched sources mid-loop.
    if [ -n "${REBUILD_EACH_ITER:-}" ]; then
        sbatch --wait -p "$PARTITION" --time=00:15:00 slurm-jobs/build.sh || true
    fi
done

echo "PARITY_HEAVY_EXHAUSTED after $MAX"
exit 1
