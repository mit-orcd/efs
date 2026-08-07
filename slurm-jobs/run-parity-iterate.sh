#!/bin/bash
# Iterate parity-suite up to MAX times; stop on first full PASS.
# Usage: ./slurm-jobs/run-parity-iterate.sh [MAX]
set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
MAX="${1:-10}"
cd "$REPO"
mkdir -p "$SHARED/logs" "$SHARED/state"

echo "=== clean rebuild ==="
sbatch --wait "$REPO/slurm-jobs/build.sh"

for i in $(seq 1 "$MAX"); do
    echo ""
    echo "======== PARITY ITER $i / $MAX ========"
    JOB=$(sbatch --parsable "$REPO/slurm-jobs/parity-suite.sh")
    echo "submitted job=$JOB"
    while squeue -j "$JOB" -h 2>/dev/null | grep -q .; do
        sleep 60
    done
    OUT="$SHARED/logs/parity-${JOB}.out"
    ERR="$SHARED/logs/parity-${JOB}.err"
    RESULT_DIR="$SHARED/logs/parity-${JOB}"
    echo "--- iter $i log tail ---"
    tail -80 "$OUT" 2>/dev/null || true
    if [ -f "$RESULT_DIR/RESULT" ] && grep -q PARITY_SUITE_OK "$RESULT_DIR/RESULT"; then
        echo "PARITY_ITER_PASS iter=$i job=$JOB"
        exit 0
    fi
    if grep -q PARITY_SUITE_OK "$OUT" 2>/dev/null; then
        echo "PARITY_ITER_PASS iter=$i job=$JOB"
        exit 0
    fi
    echo "PARITY_ITER_FAIL iter=$i job=$JOB — inspect $OUT $ERR $RESULT_DIR"
    # leave state for debugging; next iter gets fresh scratch per job
done

echo "PARITY_ITER_EXHAUSTED after $MAX"
exit 1
