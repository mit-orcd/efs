#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-overlap-hour-20261009
cd "$base"
python3 balanced-capture.py baseline 900 0
python3 balanced-analyze.py > balanced-analysis.txt
python3 balanced-capture.py overlap 900 1
python3 balanced-analyze.py > balanced-analysis.txt
mkdir final-cold
EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 0 "$base/final-cold"
python3 balanced-verify.py > cold-verify.log
bash fault-gates.sh > fault-controller.log 2>&1
