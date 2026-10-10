#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-closure-20261010
export EFS_RAFT_APPLIED_INTERVAL_MS=100 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=1 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_GROUP_DRAIN=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0
rc=0
python3 /data1/efs/w93-closure-build/tests/live/raft_read_freshness.py --publication --configuration > "$base/partition.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$base/partition-exit.txt"
exit "$rc"
