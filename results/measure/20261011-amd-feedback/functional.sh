#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
src=/data1/efs/devel-amd-feedback-20261011
mkdir -p "$out/functional"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20 EFS_RAFT_FLOW=1
export EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/functional/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 15 stat "$m" >> "$out/functional/restore.log" 2>&1 || restored=1
 done
 echo "$restored" > "$out/functional/restore.exit"
 echo "$rc" > "$out/functional/round.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
cd "$src"
./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
python3 tests/test_host_flow.py > "$out/host-flow.log" 2>&1
python3 tests/test_host_retire.py >> "$out/host-flow.log" 2>&1
python3 tests/test_host_outbox.py >> "$out/host-flow.log" 2>&1
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/functional/borrow.log" 2>&1
done
for mode in direct buffered; do
 mkdir -p "$out/functional/$mode"
 python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp \
  --root "$out/functional/$mode" --root "$out/functional/$mode" --port 24210 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
  --guarded-leases --create-lease --fuse-reply-failure --posix \
  --fresh-namespace --append-sync-boundary --append-replay-race --sync-writes \
  > "$out/functional/$mode/run.log" 2>&1
 echo 0 > "$out/functional/$mode/run.exit"
done
