#!/bin/bash
#SBATCH --job-name=efs-probe-ib
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:05:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/probe-ib-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/probe-ib-%j.err
set -euo pipefail
source /home/erbmi1/git/efs/slurm-jobs/lib-ib.sh
echo "hostname=$(hostname -s)"
efs_ib_host
ip -4 addr show | grep -E 'inet |ib|mlx' || true
ls /sys/class/infiniband 2>/dev/null || true
