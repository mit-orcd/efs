#!/bin/bash
set -euo pipefail
src=/data1/efs/release-v030-d9d39acc
out=/data1/efs/release-v030-control-20261011-d9d39acc-retry2
while [ ! -f /data1/efs/release-v030-results-20261011-d9d39acc/mounted/round.exit ]; do sleep 10; done
[ "$(cat /data1/efs/release-v030-results-20261011-d9d39acc/mounted/restore.exit)" = 0 ] || exit 1
[ "$(cat /data1/efs/release-v030-results-20261011-d9d39acc/build-and-gates.exit)" = 0 ] || exit 1
mkdir -p "$out/mounted"
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/mounted/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 15 stat "$m" >> "$out/mounted/restore.log" 2>&1 || restored=1
 done
 echo "$restored" > "$out/mounted/restore.exit"
 echo "$rc" > "$out/mounted/round.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
export EFS_TRANSPORT=tcp EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
export EFS_RAFT_FLOW=0 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0
cd "$src"
export PYTHONPATH="$src/tests/live"
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/mounted/borrow.log" 2>&1
done
failed=0
for mode in direct buffered; do
 dir="$out/mounted/$mode";mkdir -p "$dir"
 printf '%s %s START\n' "$(date -u +%FT%TZ)" "$mode" >> "$out/mounted/progress.log"
 set +e
 timeout --signal=TERM --kill-after=30 1400 python3 -u "$out/control.py" \
  --mode "$mode" --transport tcp --root "$dir" --root "$dir" --port 24210 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --mounted-packing-only --packing-clients 7 --posix-jobs 16 --posix \
  > "$dir/run.log" 2>&1
 rc=$?
 set -e
 echo "$rc" > "$dir/run.exit"
 [ "$rc" = 0 ] || failed=1
 printf '%s %s EXIT %s\n' "$(date -u +%FT%TZ)" "$mode" "$rc" >> "$out/mounted/progress.log"
done
exit "$failed"
