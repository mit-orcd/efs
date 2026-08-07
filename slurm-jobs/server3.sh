#!/bin/bash
#SBATCH --job-name=efs-s3
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/s3-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/s3-%j.err

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

PORT=1983
NODE_ID=3

read -r IB_HOST IB_IP < <(efs_ib_host)
SHORT=$(hostname -s)
echo "efs-s3 on $SHORT IB=$IB_HOST ($IB_IP):$PORT"
echo "${IB_IP}:${PORT}" > "$SHARED/state/s3.addr"
echo "$IB_IP" > "$SHARED/state/s3.host"
echo "$SHORT" > "$SHARED/state/s3.node"

WAITED=0
while [ ! -f "$SHARED/state/s1.addr" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    if [ "$WAITED" -ge 300 ]; then
        echo "Timed out waiting for server 1 address"
        exit 1
    fi
done
S1=$(cat "$SHARED/state/s1.addr")
echo "joining via IB: $S1"

DIO_ARGS=()
case "${EFS_DIO:-}" in
    off|buffered|0) DIO_ARGS+=(--no-direct-io) ;;
    on|direct|1)    DIO_ARGS+=(--direct-io) ;;
esac
# shellcheck disable=SC2086
efs_run_efsd "$SCRATCH" \
    "$REPO/efsd" --node-id "$NODE_ID" --addr "$IB_IP" --port "$PORT" \
    --storage "$STORAGE" --join "$S1" "${DIO_ARGS[@]}" ${EFS_EXTRA_ARGS:-}
