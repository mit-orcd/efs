#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
src=/data1/efs/devel-amd-feedback-20261011
[ "$(cat "$out/functional/round.exit")" = 0 ]
mkdir -p "$out/lifecycle-fault"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20 EFS_RAFT_FLOW=1
export EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_BATCH_OBS=0 EFS_RAFT_OBS=0
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/lifecycle-fault/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 15 stat "$m" >> "$out/lifecycle-fault/restore.log" 2>&1 || restored=1
 done
 echo "$restored" > "$out/lifecycle-fault/restore.exit"
 echo "$rc" > "$out/lifecycle-fault/round.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
cd "$src"
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/lifecycle-fault/borrow.log" 2>&1
done
for mode in direct buffered; do
 dir=$out/lifecycle-fault/$mode;mkdir -p "$dir"
 python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp \
  --root "$dir" --root "$dir" --port 24210 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
  --server-binary "$out/fault-binaries/efsd" --client-binary "$out/fault-binaries/efs-fuse" \
  --create-adopt-reply-loss --create-adopt-negative --create-live-retry \
  --create-negative-replay --guarded-leases --storage-session-fence-fault \
  --append-sync-boundary --append-replay-race --sync-write-fault --put-reply-fault \
  > "$dir/run.log" 2>&1
 echo 0 > "$dir/run.exit"
done
