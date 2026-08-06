#!/bin/bash
#SBATCH --job-name=efs-status
#SBATCH --partition=mit_normal
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=2G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/status-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/status-%j.err
#
# Env: STATUS_LABEL (optional tag), NUM_SERVERS (default: all s*.addr present)

set -euo pipefail
REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
LABEL="${STATUS_LABEL:-status}"
NUM_SERVERS="${NUM_SERVERS:-4}"

echo "=== $LABEL on $(hostname -s) $(date -Is) ==="
for i in $(seq 1 "$NUM_SERVERS"); do
    f="$SHARED/state/s${i}.addr"
    if [ ! -f "$f" ]; then
        echo "--- s$i: no addr file ---"
        continue
    fi
    addr=$(cat "$f")
    echo "--- status via s$i ($addr) ---"
    "$REPO/efs-mgmt" status "$addr" 2>&1 || echo "(status failed for $addr)"
done
