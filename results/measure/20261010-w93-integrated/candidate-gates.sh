#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-integrated-20261010
[ -f "$base/gates-pass.txt" ]
export EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=1 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_RAFT_APPLIED_INTERVAL_MS=100
rc=0
python3 /data1/efs/w93-integrated-build/tests/live/gc_reclamation.py --mode direct --transport tcp --port 24090 --mount /data1/efs/w93-checkpoint-replay-mnt --peer-mount /data1/efs/w93-checkpoint-replay-mnt2 --posix --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race --stop-dirty-recovery --shutdown-mutations --wal-tail-recovery > "$base/candidate-full100.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$base/candidate-full100-exit.txt"
exit "$rc"
