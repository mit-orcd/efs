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
# Always clean: ASAN builds leave instrumented .o that break normal link, and
# Makefile header deps are incomplete.
make clean
make -j4
echo "Build finished OK"
ls -la efs-fuse efsd efs-mgmt | awk '{print $5,$9}'
if nm efsd 2>/dev/null | grep -q '__asan_init'; then
    echo "ERROR: efsd still has ASAN symbols" >&2
    exit 1
fi
