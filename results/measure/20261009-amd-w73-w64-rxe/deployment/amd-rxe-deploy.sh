#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/w73-w64-rxe.lock
flock 9
base=/data1/efs
candidate=$base/gate-rxe-f6233987
out=$base/w73-w64-rxe-deploy-f6233987
mkdir "$out"
[ "$(cat "$base/w73-w64-rxe-f6233987/runner-exit")" = 0 ]
[ "$(cat "$candidate/.build_id.stamp")" = f6233987b9d1 ]
for binary in efsd efs-fuse efs-mgmt efs-bench efs-query; do
 "$candidate/$binary" --version
done
sha256sum "$candidate"/efsd "$candidate"/efs-fuse "$candidate"/efs-mgmt "$candidate"/efs-bench "$candidate"/efs-query > "$out/binary-sha256.txt"
backup=$base/deploy-backups/pre-rxe-f6233987-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$(dirname "$backup")"
echo "$backup" > "$out/backup-path.txt"
restore() {
 rc=$?
 trap - EXIT
 EFS_CLUSTER=amd EFS_DIRECT_IO=1 EFS_TRANSPORT=rdma EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 "$base/devops/start.sh" 9>&- > "$out/start.log" 2>&1 || { cat "$out/start.log"; exit 90; }
 echo "$rc" > "$out/deploy-exit"
 exit "$rc"
}
trap restore EXIT
for m in "$base/mnt" "$base/mnt2"; do
 if ! "$base/src/scripts/client.sh" stop "$m"; then
  mountpoint -q "$m" && exit 1
  sleep 1
  pgrep -f "^./efs-fuse .* $m$" && exit 1
 fi
done
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
[ "$(echo "$pids" | wc -l)" = 4 ]
kill -TERM $pids
for i in $(seq 1 60); do alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
[ "$alive" = 0 ] || exit 1
mv "$base/src" "$backup"
mv "$candidate" "$base/src"
echo 'Installed accepted RXE candidate without rebuilding binaries'
