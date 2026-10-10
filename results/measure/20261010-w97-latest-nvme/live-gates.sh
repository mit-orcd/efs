#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-latest-20261010
build=/data1/efs/w97-latest-build
[ "$(cat "$base/native-exit.txt")" = 0 ]
export EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_RAFT_APPLIED_INTERVAL_MS=10
rc=0
python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --port 24590 --mount /data1/efs/w97-latest-test-mnt --peer-mount /data1/efs/w97-latest-test-mnt2 --posix --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race --stop-dirty-recovery --shutdown-mutations --wal-tail-recovery > "$base/live-gates.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$base/live-exit.txt"
exit "$rc"
