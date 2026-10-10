#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-closure-20261010
build=/data1/efs/w93-closure-build
cd "$base"
# Native build must have a recorded successful exit; never run an incomplete build.
[ "$(cat native-exit.txt)" = 0 ]
export EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0
run_gate() {
 name=$1;interval=$2;port=$3;shift 3
 rc=0
 EFS_RAFT_APPLIED_INTERVAL_MS="$interval" python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --port "$port" --mount /data1/efs/w93-checkpoint-replay-mnt --peer-mount /data1/efs/w93-checkpoint-replay-mnt2 "$@" > "$name.log" 2>&1 || rc=$?
 printf '%s\n' "$rc" > "$name-exit.txt"
 [ "$rc" = 0 ]
}
if [ "${1:-}" = remaining ]; then
 [ "$(cat full100-exit.txt)" = 0 ]
else
 run_gate full100 100 22790 --posix --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race --stop-dirty-recovery --shutdown-mutations --wal-tail-recovery
fi
run_gate kv1000 1000 22890 --kv-wal-persistence-only
run_gate marker1000 1000 22990 --applied-checkpoint-replay-only
printf 'all private durability gates PASS\n' > gates-pass.txt
