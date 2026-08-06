#!/bin/bash
#SBATCH --job-name=efs-smoke
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-meta-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-meta-%j.err
set -euo pipefail
cd /home/erbmi1/git/efs
./tests/test_erasure
./tests/test_placement
./tests/test_integration
./tests/test_quota
echo SMOKE_OK
