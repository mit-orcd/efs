#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-read-pressure-hour-20261009
cd /data1/efs/w93-read-build
make -j4 EFS_GIT_ID=437b7890-read-pressure EFS_GIT_DESCRIBE=437b7890 EFS_GIT_BRANCH=devel-persistence-pressure efsd efs-fuse efs-mgmt efs-query efs-bench tests/test_raft tests/test_raft_store tests/test_wire > "$base/build-fixed.log" 2>&1
{ ./tests/test_raft; ./tests/test_raft_store; ./tests/test_wire; python3 tests/test_host_retire.py; python3 tests/test_host_checkpoint.py; python3 tests/test_fence_read.py; python3 tests/test_writer_fence_merge.py; } > "$base/core-fixed.log" 2>&1
cd "$base"
bash deploy-fixed.sh > deployment-fixed.log 2>&1
python3 /data1/efs/w93-read-build/tests/posix/posix_suite.py /data1/efs/mnt --jobs 4 --timeout-s 30 --results "$base/posix-fixed.tsv" > posix-fixed.log 2>&1
python3 /data1/efs/w93-read-build/tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 --parent w93-read-pressure-peer-fixed --results "$base/peer-fixed.tsv" > peer-fixed.log 2>&1
for i in $(seq 1 20); do
 python3 /data1/efs/w93-read-build/tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 --filter peer_append_while_truncate --parent w93-fixed-append-$i --results "$base/append-fixed-$i.tsv" > "$base/append-fixed-$i.log" 2>&1
done
python3 balanced-capture.py late 240 0
python3 balanced-capture.py early 240 1
python3 balanced-capture.py late2 240 0
python3 balanced-analyze.py > balanced-analysis.txt
mkdir final-cold
EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 0 "$base/final-cold"
python3 balanced-verify.py > cold-verify.log
bash fault-gates.sh > fault-controller.log 2>&1
