#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-integrated-20261010
cd "$base"
[ -f gates-pass.txt ]
[ "$(cat candidate-full100-exit.txt)" = 0 ]
[ "$(cat partition-exit.txt)" = 0 ]
python3 balanced-capture.py control1 900 10 reuse > control1-controller.log 2>&1
python3 balanced-capture.py candidate1 900 100 > candidate1-controller.log 2>&1
python3 balanced-capture.py candidate2 900 100 > candidate2-controller.log 2>&1
python3 balanced-capture.py control2 900 10 > control2-controller.log 2>&1
python3 balanced-analyze.py > analysis.log
python3 resource-summary.py > resource-summary.log
python3 disk-summary.py > disk-summary.log
# Force both readers cold on preserved stores, restore unchanged defaults.
EFS_RAFT_APPLIED_INTERVAL_MS=10 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 0 "$base" > restore-controller.log 2>&1
python3 balanced-verify.py > cold-verification.log
printf '0\n' > comparison-exit.txt
