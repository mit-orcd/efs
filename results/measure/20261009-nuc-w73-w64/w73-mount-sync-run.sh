#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-gates-v2.lock
flock -n 9 || exit 73
build=/data1/efs/gate-closure-build
out=/data1/efs/w73-mount-sync-3e1d4c6e
mkdir -p "$out"
restore() {
 rc=$?
 trap - EXIT
 echo "RESTORE installed service; gate runner exit=$rc"
 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp /data1/efs/devops/start.sh 9>&- >"$out/restoration.log" 2>&1 || { cat "$out/restoration.log"; exit 90; }
 echo "$rc" > "$out/runner-exit"
 exit "$rc"
}
# Drain installed mounts without disposable-cluster force-discard behavior.
for m in /data1/efs/mnt /data1/efs/mnt2; do
 if ! /data1/efs/src/scripts/client.sh stop "$m"; then
  mountpoint -q "$m" && exit 1
  sleep 1
  pgrep -f "^./efs-fuse .* $m$" && exit 1
 fi
done
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
kill -TERM $pids
for i in $(seq 1 60); do alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
[ "$alive" = 0 ] || exit 1
trap restore EXIT
export EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_TRANSPORT=tcp
failed=0
for mode in direct buffered; do
 echo "START $mode TCP $(date -u +%FT%TZ)"
 if python3 "$build/tests/live/w73_mount_sync.py" --mode "$mode" --transport tcp --root /data1/efs --root /data2/efs --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --sync-writes --mount-sync > "$out/$mode-tcp.log" 2>&1; then
  echo PASS > "$out/$mode-tcp.status"
 else
  echo FAIL > "$out/$mode-tcp.status"; failed=1
 fi
 tail -20 "$out/$mode-tcp.log"
 mountpoint -q /data1/efs/mnt && exit 1
 mountpoint -q /data1/efs/mnt2 && exit 1
done
exit "$failed"
