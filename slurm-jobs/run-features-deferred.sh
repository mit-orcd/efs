#!/bin/bash
# Deferred complete features run: rebuild, make test, then multi-node harness.
# Submit with: sbatch --begin=YYYY-MM-DDTHH:MM:SS slurm-jobs/run-features-deferred.sh
#SBATCH --job-name=efs-features
#SBATCH --partition=mit_normal
#SBATCH --time=02:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/features-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/features-%j.err

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
# Keep summary outside SHARED/logs — run.sh wipes that directory.
LOG="$SHARED/features-run-${SLURM_JOB_ID}.log"
mkdir -p "$SHARED/logs" "$SCRATCH"
cd "$REPO"

cleanup() {
    rm -rf "$SCRATCH"
    # Integration tests use fixed /tmp paths; scrub leftovers from this node.
    rm -rf /tmp/efs_test /tmp/efs_quota_test /tmp/efs_migrate_test \
           /tmp/efs_directio_test /tmp/efs_rejoin_test /tmp/efs_query_test \
           /tmp/efs_list_exports_test /tmp/efs_fuse_test 2>/dev/null || true
}
trap cleanup EXIT

exec > >(tee -a "$LOG") 2>&1

echo "=== features deferred run on $(hostname) at $(date) ==="
echo "job=$SLURM_JOB_ID scratch=$SCRATCH log=$LOG"

echo "=== 1) rebuild ==="
make -j4
echo "rebuild OK"

echo "=== 2) make test (cluster feature suite) ==="
export TMPDIR="$SCRATCH/tmp"
mkdir -p "$TMPDIR"
make test
echo "make test OK"

echo "=== 3) multi-node harness ./slurm-jobs/run.sh ==="
# run.sh rebuilds again and drives the 3-server + 2-client cluster.
./slurm-jobs/run.sh
echo "run.sh OK"

echo "=== ALL PASSED at $(date) ==="