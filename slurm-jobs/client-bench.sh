#!/bin/bash
#SBATCH --job-name=efs-cli-bench
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-bench-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/client-bench-%j.err
#
# Cluster client bench (discover from seed, fanout like mount):
#   MODE=net   (default): efs-bench SEED --time TIME_SEC   (ACK/discard)
#   MODE=store:           efs-bench SEED --size SIZE        (real PUT store)
#
# Env: MODE, TIME_SEC, SIZE (e.g. 256M), SEED, NUM_SERVERS

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

SCRATCH=$(efs_job_scratch)
MODE="${MODE:-net}"
TIME_SEC="${TIME_SEC:-5}"
SIZE="${SIZE:-256M}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_DIR="${PROF_DIR:-}"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH" "$SHARED/logs"
[ -n "$PROF_DIR" ] && mkdir -p "$PROF_DIR"

cleanup() {
    set +e
    if [ -n "${PROF_DIR:-}" ] && [ -n "${SCRATCH:-}" ]; then
        for f in client.perf.data client.perf.data.report.txt client.perf.data.hotpath.txt; do
            [ -f "$SCRATCH/$f" ] && cp -f "$SCRATCH/$f" "$PROF_DIR/" 2>/dev/null || true
        done
        # normalize names
        [ -f "$PROF_DIR/client.perf.data.report.txt" ] && \
            mv -f "$PROF_DIR/client.perf.data.report.txt" "$PROF_DIR/client.report.txt" 2>/dev/null || true
        [ -f "$PROF_DIR/client.perf.data.hotpath.txt" ] && \
            mv -f "$PROF_DIR/client.perf.data.hotpath.txt" "$PROF_DIR/client.hotpath.txt" 2>/dev/null || true
    fi
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== client-bench on $(hostname -s) IB=$CLIENT_IB ($CLIENT_IP) mode=$MODE ==="

SEED="${SEED:-}"
if [ -z "$SEED" ]; then
    efs_wait_addr 1 300
    SEED=$(cat "$SHARED/state/s1.addr")
fi
echo "seed=$SEED"

for i in $(seq 1 "$NUM_SERVERS"); do
    efs_wait_addr "$i" 300
    addr=$(cat "$SHARED/state/s${i}.addr")
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 1
        WAITED=$((WAITED + 1))
        [ "$WAITED" -ge 120 ] && { echo "timeout $addr"; exit 1; }
    done
done

PERF_ARGS=()
if [ "${EFS_BENCH_PERF:-0}" = "1" ]; then
    PERF_ARGS+=(--perf)
    export EFS_PERF_PATH="$SCRATCH/client.perf.data"
fi

case "$MODE" in
    net|discard)
        echo "running: efs-bench $SEED --time $TIME_SEC ${PERF_ARGS[*]:-}"
        "$REPO/efs-bench" "$SEED" --time "$TIME_SEC" "${PERF_ARGS[@]}"
        ;;
    store|put)
        echo "running: efs-bench $SEED --size $SIZE ${PERF_ARGS[*]:-}"
        "$REPO/efs-bench" "$SEED" --size "$SIZE" "${PERF_ARGS[@]}"
        ;;
    *)
        echo "ERROR: MODE must be net or store (got $MODE)" >&2
        exit 1
        ;;
esac
echo "client-bench done"
