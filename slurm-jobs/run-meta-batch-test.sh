#!/bin/bash
#SBATCH --job-name=efs-mbt
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --cpus-per-task=2
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/mbt-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/mbt-%j.err
set -e
cd /home/erbmi1/git/efs
gcc -O1 -g -std=c99 -Wall -D_GNU_SOURCE -Iinclude -Isrc/common -Isrc/client \
  -o tests/test_meta_batch tests/test_meta_batch.c libefs.a -lpthread -lm -ldl
./tests/test_meta_batch
echo TEST_OK
