# Shared multi-node harness helpers. Source from orchestrators / job scripts.

# Node-local root for this Slurm job: /scratch/efs-testing/<jobid>
# All compute-node data (server storage, FUSE mounts, perf, temp) must live
# under this directory. Shared cross-node state stays on /orcd/scratch/...
EFS_SCRATCH_ROOT="${EFS_SCRATCH_ROOT:-/scratch/efs-testing}"

efs_job_scratch() {
    if [ -z "${SLURM_JOB_ID:-}" ]; then
        echo "ERROR: SLURM_JOB_ID unset; refuse to use scratch without a job id" >&2
        return 1
    fi
    if [ ! -d /scratch ]; then
        echo "ERROR: /scratch missing on $(hostname); required for node-local data" >&2
        return 1
    fi
    mkdir -p "$EFS_SCRATCH_ROOT" 2>/dev/null || {
        echo "ERROR: cannot create $EFS_SCRATCH_ROOT on $(hostname)" >&2
        return 1
    }
    printf '%s/%s\n' "$EFS_SCRATCH_ROOT" "$SLURM_JOB_ID"
}

# node1611.ib:1981 or node1611.ib → Slurm NodeName (node1611)
efs_slurm_node() {
    local h=${1%:*}
    h=${h%.ib}
    printf '%s\n' "$h"
}

# Wait until SHARED/state/sN.addr exists (and optionally is non-empty).
efs_wait_addr() {
    local n=$1
    local max_sec=${2:-300}
    local shared=${SHARED:-/orcd/scratch/orcd/001/erbmi1/efs}
    local f="$shared/state/s${n}.addr"
    local waited=0
    while [ ! -s "$f" ]; do
        sleep 2
        waited=$((waited + 2))
        if [ "$waited" -ge "$max_sec" ]; then
            echo "Timed out waiting for $f" >&2
            return 1
        fi
    done
}

# Fail unless server short hostnames s1..sN are all different.
# Usage: efs_assert_distinct_servers [count]   (default 3)
efs_assert_distinct_servers() {
    local shared=${SHARED:-/orcd/scratch/orcd/001/erbmi1/efs}
    local n=${1:-3}
    local hosts=()
    local i h j
    for i in $(seq 1 "$n"); do
        if [ ! -s "$shared/state/s${i}.host" ]; then
            echo "ERROR: missing server host file s${i}.host" >&2
            return 1
        fi
        h=$(efs_slurm_node "$(cat "$shared/state/s${i}.host")")
        if [ -z "$h" ]; then
            echo "ERROR: empty host for server $i" >&2
            return 1
        fi
        hosts+=("$h")
        echo "  s${i}=$h"
    done
    echo "server nodes: ${hosts[*]}"
    for i in $(seq 0 $((n - 1))); do
        for j in $(seq $((i + 1)) $((n - 1))); do
            if [ "${hosts[$i]}" = "${hosts[$j]}" ]; then
                echo "ERROR: servers must run on distinct nodes (s$((i+1))=s$((j+1))=${hosts[$i]})" >&2
                return 1
            fi
        done
    done
}

# Run efsd in the background so EXIT/TERM can remove node-local /scratch.
# Usage: efs_run_efsd <storage_dir> <args...>
efs_run_efsd() {
    EFS_SCRATCH_CLEANUP=$1
    shift
    EFS_EFSD_PID=""
    _efs_efsd_cleanup() {
        set +e
        if [ -n "${EFS_EFSD_PID:-}" ]; then
            kill -TERM "$EFS_EFSD_PID" 2>/dev/null || true
            local i
            for i in $(seq 1 40); do
                kill -0 "$EFS_EFSD_PID" 2>/dev/null || break
                sleep 0.25
            done
            kill -KILL "$EFS_EFSD_PID" 2>/dev/null || true
            wait "$EFS_EFSD_PID" 2>/dev/null || true
            EFS_EFSD_PID=""
        fi
        if [ -n "${EFS_PERF_COPY:-}" ] && [ -n "${EFS_SCRATCH_CLEANUP:-}" ] && \
           [ -f "${EFS_SCRATCH_CLEANUP}/server1.perf.data" ]; then
            cp -f "${EFS_SCRATCH_CLEANUP}/server1.perf.data" "$EFS_PERF_COPY" \
                2>/dev/null || true
        fi
        if [ -n "${EFS_SCRATCH_CLEANUP:-}" ]; then
            echo "cleaning /scratch: $EFS_SCRATCH_CLEANUP"
            rm -rf "$EFS_SCRATCH_CLEANUP"
            EFS_SCRATCH_CLEANUP=""
        fi
    }
    trap _efs_efsd_cleanup EXIT TERM INT
    "$@" &
    EFS_EFSD_PID=$!
    wait "$EFS_EFSD_PID"
    local rc=$?
    EFS_EFSD_PID=""
    return "$rc"
}
