#!/bin/bash
#SBATCH --job-name=efs-s4d
#SBATCH --partition=mit_normal
#SBATCH --time=04:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/s4d-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/s4d-%j.err
#
# One efsd with 4 local storage roots. Env: SERVER_ID=1|2|3

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

SERVER_ID="${SERVER_ID:?SERVER_ID required}"
PORT=$((1980 + SERVER_ID))
NODE_ID="$SERVER_ID"

SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH/d1" "$SCRATCH/d2" "$SCRATCH/d3" "$SCRATCH/d4" \
         "$SHARED/state" "$SHARED/logs"
STORAGE="$SCRATCH/d1,$SCRATCH/d2,$SCRATCH/d3,$SCRATCH/d4"

read -r IB_HOST IB_IP < <(efs_ib_host)
SHORT=$(hostname -s)
echo "efs-s${SERVER_ID} on $SHORT IB=$IB_HOST ($IB_IP):$PORT storage=$STORAGE"
echo "${IB_HOST}:${PORT}" > "$SHARED/state/s${SERVER_ID}.addr"
echo "$IB_HOST" > "$SHARED/state/s${SERVER_ID}.host"
echo "$SHORT" > "$SHARED/state/s${SERVER_ID}.node"

JOIN_ARGS=()
if [ "$SERVER_ID" -gt 1 ]; then
    efs_wait_addr 1 600
    S1=$(cat "$SHARED/state/s1.addr")
    echo "joining via IB: $S1"
    JOIN_ARGS=(--join "$S1")
fi

DIO_ARGS=()
case "${EFS_DIO:-}" in
    off|buffered|0) DIO_ARGS+=(--no-direct-io) ;;
    on|direct|1)    DIO_ARGS+=(--direct-io) ;;
esac

# shellcheck disable=SC2086
efs_run_efsd "$SCRATCH" \
    "$REPO/efsd" --node-id "$NODE_ID" --addr "$IB_HOST" --port "$PORT" \
    --storage "$STORAGE" --quota "${EFS_QUOTA:-200G}" \
    "${DIO_ARGS[@]}" "${JOIN_ARGS[@]}" ${EFS_EXTRA_ARGS:-}
