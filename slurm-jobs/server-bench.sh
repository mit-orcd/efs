#!/bin/bash
#SBATCH --job-name=efs-srv-bench
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/server-bench-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/server-bench-%j.err
#
# Local storage path saturate: efsd --bench PATH --time SEC
#
# Usage:
#   sbatch slurm-jobs/server-bench.sh
#   TIME_SEC=5 WRITERS=16 sbatch slurm-jobs/server-bench.sh
#   # or with explicit path (must be on the compute node's /scratch):
#   BENCH_PATH=/scratch/efs-testing/$SLURM_JOB_ID/storage TIME_SEC=5 \
#     sbatch slurm-jobs/server-bench.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

SCRATCH=$(efs_job_scratch)
TIME_SEC="${TIME_SEC:-5}"
WRITERS="${WRITERS:-16}"
BENCH_PATH="${BENCH_PATH:-$SCRATCH/storage}"

rm -rf "$SCRATCH"
mkdir -p "$BENCH_PATH" "$SHARED/logs"

cleanup() {
    set +e
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

echo "=== server-bench host=$(hostname -s) path=$BENCH_PATH time=$TIME_SEC writers=$WRITERS ==="

DIO_ARGS=()
case "${EFS_DIO:-}" in
    on|direct|1)    DIO_ARGS=(--direct-io) ;;
    off|buffered|0) DIO_ARGS=(--no-direct-io) ;;
esac

"$REPO/efsd" --bench "$BENCH_PATH" --time "$TIME_SEC" --writers "$WRITERS" \
    "${DIO_ARGS[@]}"
echo "server-bench done"
