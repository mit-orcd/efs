#!/bin/bash
# Compare single-dd bandwidth: 3×1-disk vs 3×4-disk, plus net-only ceiling.
set -euo pipefail
REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
PARTITION="${PARTITION:-mit_normal}"
FILE_GIB="${FILE_GIB:-2}"
SKIP_BUILD="${SKIP_BUILD:-0}"
cd "$REPO"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"
PROF="$SHARED/profile/dd-bw-compare-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$PROF" "$SHARED/state" "$SHARED/logs"
chmod +x "$REPO/slurm-jobs/client-dd-bw.sh"
LEDGER="$PROF/ledger.tsv"
printf 'config\twrite_GiB_s\tread_GiB_s\tstore_GiB_s\tnet_GiB_s\n' >"$LEDGER"

SERVER_JOBS=()
EXCLUDE=""

if [ "$SKIP_BUILD" != "1" ]; then
    sbatch --wait -p "$PARTITION" --time=00:15:00 slurm-jobs/build.sh
fi

bringup() {
    local mode=$1
    rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
    SERVER_JOBS=()
    EXCLUDE=""
    local sid job node
    for sid in 1 2 3; do
        local excl_args=()
        [ -n "$EXCLUDE" ] && excl_args=(--exclude="$EXCLUDE")
        if [ "$mode" = "4disk" ]; then
            job=$(sbatch --parsable -p "$PARTITION" --time=00:45:00 --no-requeue \
                --cpus-per-task=8 --mem=16G "${excl_args[@]}" \
                --export=ALL,SERVER_ID="$sid",EFS_DIO=off,EFS_QUOTA=50G \
                slurm-jobs/server-4disk.sh)
        else
            job=$(sbatch --parsable -p "$PARTITION" --time=00:45:00 --no-requeue \
                --cpus-per-task=4 --mem=8G "${excl_args[@]}" \
                --export=ALL,EFS_DIO=off \
                "slurm-jobs/server${sid}.sh")
        fi
        SERVER_JOBS+=("$job")
        efs_wait_addr "$sid" 600
        node=$(cat "$SHARED/state/s${sid}.node")
        if [ -z "$EXCLUDE" ]; then EXCLUDE="$node"; else EXCLUDE="${EXCLUDE},${node}"; fi
        echo "  $mode s$sid=$node job=$job"
    done
    for job in "${SERVER_JOBS[@]}"; do
        local log
        log=$(ls -t "$SHARED/logs"/s*-"${job}".out "$SHARED/logs"/s4d-"${job}".out 2>/dev/null | head -1 || true)
        for _ in $(seq 1 60); do
            grep -q "listening on" "$log" 2>/dev/null && break
            sleep 2
        done
    done
    sleep 2
}

teardown() {
    local j
    for j in "${SERVER_JOBS[@]:-}"; do scancel "$j" 2>/dev/null || true; done
    SERVER_JOBS=()
    sleep 2
    rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node
}

run_one() {
    local label=$1
    local export_name="cmp-${label}"
    local prof="$PROF/$label"
    mkdir -p "$prof"
    local MKFS NET CLIENT net_line net_gib w r s
    MKFS=$(sbatch --parsable -p "$PARTITION" --time=00:10:00 --exclude="$EXCLUDE" \
        --export=ALL,EXPORT_NAME="$export_name" slurm-jobs/mkfs-ib.sh)
    while squeue -h -j "$MKFS" 2>/dev/null | grep -q .; do sleep 30; done

    NET=$(sbatch --parsable --wait -p "$PARTITION" --time=00:15:00 --exclude="$EXCLUDE" \
        --cpus-per-task=8 --mem=8G \
        --export=ALL,MODE=net,TIME_SEC=5,NUM_SERVERS=3,PROF_DIR="$prof/net" \
        slurm-jobs/client-bench.sh)
    net_line=$(grep 'BENCH_OK kind=net' "$SHARED/logs/client-bench-${NET}.out" 2>/dev/null | tail -1 || true)
    net_gib=$(echo "$net_line" | sed -n 's/.*GiB_s=\([0-9.]*\).*/\1/p')
    echo "$net_line" | tee "$prof/net.txt"

    CLIENT=$(sbatch --parsable -p "$PARTITION" --time=00:45:00 --exclude="$EXCLUDE" \
        --cpus-per-task=8 --mem=16G \
        --export=ALL,EXPORT_NAME="$export_name",PROF_ROOT="$prof",NUM_SERVERS=3,FILE_GIB="$FILE_GIB",DO_STORE_BENCH=1,EFS_FUSE_PERF=0,LABEL="$label" \
        slurm-jobs/client-dd-bw.sh)
    touch "$prof/GO"
    while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do sleep 60; done
    w=$(grep -oP 'write_GiB_s=\K[0-9.]+' "$prof"/client-*/SUMMARY.txt 2>/dev/null | head -1 || true)
    r=$(grep -oP 'read_GiB_s=\K[0-9.]+' "$prof"/client-*/SUMMARY.txt 2>/dev/null | head -1 || true)
    s=$(grep -oP 'logical_GiB_s=\K[0-9.]+' "$prof"/client-*/SUMMARY.txt 2>/dev/null | head -1 || true)
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "${w:-0}" "${r:-0}" "${s:-0}" "${net_gib:-0}" | tee -a "$LEDGER"
    cat "$prof"/client-*/SUMMARY.txt 2>/dev/null | tee "$prof/SUMMARY.txt" || true
}

echo "=== 1-disk cluster ==="
bringup 1disk
run_one "1disk"
teardown

echo "=== 4-disk cluster ==="
bringup 4disk
run_one "4disk"
teardown

column -t -s $'\t' "$LEDGER" | tee "$PROF/FINAL.txt"
echo "DONE PROF=$PROF"
