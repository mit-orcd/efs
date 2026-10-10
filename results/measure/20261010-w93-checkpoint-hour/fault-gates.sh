#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-checkpoint-hour-20261010
build=/data1/efs/w93-checkpoint-build
restore_service() {
# Verify private harness relinquished mounts before restoring preserved stores.
if mountpoint -q /data1/efs/mnt || mountpoint -q /data1/efs/mnt2; then
 echo 'private gate retained a mount; automatic service restore refused' >&2;exit 1
fi
mkdir -p "$base/fault-restored"
EFS_RAFT_APPLIED_INTERVAL_MS=10 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_BATCH_OBS=0 \
 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp \
 bash "$base/start-preserved.sh" > "$base/fault-restored/start.log" 2>&1
mountpoint -q /data1/efs/mnt
mountpoint -q /data1/efs/mnt2
}
mkdir -p "$base/fault-stop"
bash "$base/stop.sh" 1 0 "$base/fault-stop"
raw_rc=0
python3 "$base/raw-sync.py" > "$base/raw-sync.log" 2>&1 || raw_rc=$?
printf '%s\n' "$raw_rc" > "$base/raw-sync-exit.txt"
if [ "$raw_rc" != 0 ]; then restore_service;exit "$raw_rc";fi
# Private fixture owns unique stores; service stores remain stopped and intact.
set +e
EFS_RAFT_APPLIED_INTERVAL_MS=100 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 \
 python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp \
 --port 22190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
 --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race \
 --stop-dirty-recovery --shutdown-mutations --wal-tail-recovery > "$base/fault-live.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$base/fault-exit.txt"
if [ "$rc" = 0 ]; then
 set +e
 EFS_RAFT_APPLIED_INTERVAL_MS=1000 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 \
 python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --port 22290 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --kv-wal-persistence-only > "$base/kv-wal-fault-live.log" 2>&1
 rc=$?
 set -e
 printf '%s\n' "$rc" > "$base/kv-wal-fault-exit.txt"
fi

if [ "$rc" = 0 ]; then
 set +e
 EFS_RAFT_APPLIED_INTERVAL_MS=1000 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 \
 python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --port 22390 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --applied-checkpoint-replay-only > "$base/applied-replay-final-live.log" 2>&1
 rc=$?
 set -e
 printf '%s\n' "$rc" > "$base/applied-replay-final-exit.txt"
fi

restore_service
exit "$rc"
