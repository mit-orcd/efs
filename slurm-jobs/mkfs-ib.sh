#!/bin/bash
#SBATCH --job-name=efs-mkfs
#SBATCH --partition=mit_normal
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=2G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/mkfs-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/mkfs-%j.err

set -euo pipefail
REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
EXPORT_NAME="${EXPORT_NAME:-streamexport}"

S1=$(cat "$SHARED/state/s1.addr")
echo "mkfs $EXPORT_NAME on $S1"
OUT=$("$REPO/efs-mgmt" mkfs "$S1" "$EXPORT_NAME" 2>&1) || true
echo "$OUT"
echo "$OUT" | grep -q "created\|already exists"
