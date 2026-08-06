#!/bin/bash
#SBATCH --job-name=efs-asan-build
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --cpus-per-task=4
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/build-asan-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/build-asan-%j.err
set -e
cd /home/erbmi1/git/efs
make clean
make -j4 \
  CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-stringop-truncation -Wno-format-truncation -fsanitize=address' \
  LDFLAGS='-fsanitize=address -lpthread -lm -ldl'
echo ASAN_BUILD_OK
