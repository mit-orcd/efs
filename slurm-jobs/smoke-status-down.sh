#!/bin/bash
#SBATCH --job-name=efs-stdown
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --cpus-per-task=2
#SBATCH --mem=2G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/stdown-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/stdown-%j.err
set -euo pipefail
REPO=/home/erbmi1/git/efs
SCRATCH=/scratch/efs-testing/$SLURM_JOB_ID
mkdir -p $SCRATCH/{s1,s2,s3}
cd $REPO
make -j2 efsd efs-mgmt
./efsd --node-id 1 --addr 127.0.0.1 --port 19701 --storage $SCRATCH/s1 --writers 2 >$SCRATCH/s1.log 2>&1 &
echo $! >$SCRATCH/s1.pid
sleep 1
./efsd --node-id 2 --addr 127.0.0.1 --port 19702 --storage $SCRATCH/s2 --writers 2 --join 127.0.0.1:19701 >$SCRATCH/s2.log 2>&1 &
echo $! >$SCRATCH/s2.pid
./efsd --node-id 3 --addr 127.0.0.1 --port 19703 --storage $SCRATCH/s3 --writers 2 --join 127.0.0.1:19701 >$SCRATCH/s3.log 2>&1 &
echo $! >$SCRATCH/s3.pid
sleep 2
echo '=== all up ==='
./efs-mgmt status 127.0.0.1:19701 | tee $SCRATCH/st0.txt
grep -q 'Cluster state: OK' $SCRATCH/st0.txt
grep -q ' up' $SCRATCH/st0.txt
kill -9 $(cat $SCRATCH/s2.pid)
rm -f $SCRATCH/s2.pid
sleep 0.5
echo '=== s2 killed ==='
set +e
./efs-mgmt status 127.0.0.1:19701 | tee $SCRATCH/st1.txt
RC=$?
set -e
cat $SCRATCH/st1.txt
grep -q 'DOWN' $SCRATCH/st1.txt
grep -q 'DEGRADED' $SCRATCH/st1.txt
test "$RC" -ne 0
echo STATUS_DOWN_OK
kill $(cat $SCRATCH/s*.pid) 2>/dev/null || true
rm -rf $SCRATCH
