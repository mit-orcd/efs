# Shared multi-node harness helpers. Source from orchestrators / job scripts.

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

# Fail unless the three server short hostnames are all different.
efs_assert_distinct_servers() {
    local shared=${SHARED:-/orcd/scratch/orcd/001/erbmi1/efs}
    local a b c
    a=$(efs_slurm_node "$(cat "$shared/state/s1.host")")
    b=$(efs_slurm_node "$(cat "$shared/state/s2.host")")
    c=$(efs_slurm_node "$(cat "$shared/state/s3.host")")
    echo "server nodes: s1=$a s2=$b s3=$c"
    if [ -z "$a" ] || [ -z "$b" ] || [ -z "$c" ]; then
        echo "ERROR: missing server host file(s)" >&2
        return 1
    fi
    if [ "$a" = "$b" ] || [ "$a" = "$c" ] || [ "$b" = "$c" ]; then
        echo "ERROR: servers must run on three distinct nodes (got $a $b $c)" >&2
        return 1
    fi
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
