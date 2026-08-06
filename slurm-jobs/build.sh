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
# Makefile lacks header deps; force client/common rebuild when headers move.
need_client=0
if [ ! -f src/client/client.o ] || [ include/efs/common.h -nt src/client/client.o ] \
   || [ src/client/client_internal.h -nt src/client/client.o ]; then
    need_client=1
fi
if [ "$need_client" = 1 ]; then
    echo "Forcing client object rebuild (header newer than client.o)"
    rm -f src/client/*.o efs-fuse libefs.a
fi
make -j4
echo "Build finished OK"
ls -la efs-fuse efsd efs-mgmt | awk '{print $5,$9}'
