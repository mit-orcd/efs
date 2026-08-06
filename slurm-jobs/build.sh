#!/bin/bash
#SBATCH --job-name=efs-build
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/build-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/build-%j.err

set -e

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
mkdir -p "$SHARED/logs"
cd "$REPO"

echo "Building on $(hostname) at $(date)"
make -j4
echo "Build finished OK"
