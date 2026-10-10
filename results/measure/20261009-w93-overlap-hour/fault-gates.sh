#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-overlap-hour-20261009
build=/data1/efs/w93-overlap-build
mkdir -p "$base/fault-stop"
bash "$base/stop.sh" 1 0 "$base/fault-stop"
# Private fixture owns unique stores; service stores remain stopped and intact.
set +e
EFS_RAFT_FLOW=1 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=1 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 \
 python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp \
 --port 22190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
 --sync-writes --sync-write-fault --append-sync-boundary --append-replay-race \
 --stop-dirty-recovery --shutdown-mutations --wal-tail-recovery > "$base/fault-live.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$base/fault-exit.txt"
# Verify private harness relinquished mounts before restoring preserved stores.
if mountpoint -q /data1/efs/mnt || mountpoint -q /data1/efs/mnt2; then
 echo 'private gate retained a mount; automatic service restore refused' >&2;exit 1
fi
mkdir -p "$base/fault-restored"
EFS_RAFT_FLOW=1 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_BATCH_OBS=0 \
 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp \
 /data1/efs/devops/start.sh > "$base/fault-restored/start.log" 2>&1
mountpoint -q /data1/efs/mnt
mountpoint -q /data1/efs/mnt2
exit "$rc"
