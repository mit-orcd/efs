#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-latest-20261010
cd "$base"
bash live-gates.sh
bash deploy.sh > deploy.log 2>&1
python3 balanced-capture.py continuous 3000 10 reuse > continuous-controller.log 2>&1
python3 continuous-analyze.py > analysis.log
EFS_RAFT_APPLIED_INTERVAL_MS=10 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 0 "$base" > restore.log 2>&1
python3 cold-verify.py > cold-verification.log
touch STOP-NVME
printf '0\n' > measurement-exit.txt
