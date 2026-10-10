#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-read-pressure-hour-20261009
cd "$base"
mv early early-start-failed
python3 balanced-capture.py early 240 1 reuse
python3 balanced-capture.py late2 180 0
python3 balanced-analyze.py > balanced-analysis.txt
mkdir final-cold
EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 0 "$base/final-cold"
python3 balanced-verify.py > cold-verify.log
bash fault-gates.sh > fault-controller.log 2>&1
