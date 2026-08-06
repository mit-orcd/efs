#!/bin/bash
#SBATCH --job-name=efs-s1
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/s1-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/s1-%j.err

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
STORAGE="$SCRATCH/storage"
rm -rf "$SCRATCH"
mkdir -p "$STORAGE" "$SHARED/state" "$SHARED/logs"

PORT=1981
NODE_ID=1

read -r IB_HOST IB_IP < <(efs_ib_host)
SHORT=$(hostname -s)
echo "efs-s1 on $SHORT IB=$IB_HOST ($IB_IP):$PORT"
# Advertise hostname.ib so peers/clients connect over InfiniBand.
echo "${IB_HOST}:${PORT}" > "$SHARED/state/s1.addr"
echo "$IB_HOST" > "$SHARED/state/s1.host"
echo "$SHORT" > "$SHARED/state/s1.node"

efs_run_efsd "$SCRATCH" \
    "$REPO/efsd" --node-id "$NODE_ID" --addr "$IB_HOST" --port "$PORT" --storage "$STORAGE"
