#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-replication-baseline-20261009
[ -f "$base/complete.json" ]
pid=$(cat "$base/loader.pid")
if kill -0 "$pid" 2>/dev/null; then echo 'loader active; refuse restart' >&2;exit 1;fi
for mount in /data1/efs/mnt /data1/efs/mnt2; do
    /data1/efs/src/scripts/client.sh stop "$mount" || true
    if mountpoint -q "$mount" || pgrep -f "^[^ ]*efs-fuse .* $mount$" >/dev/null; then
        echo "client/mount retained: $mount" >&2;exit 1
    fi
done
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
kill -TERM $pids
alive=1
for i in $(seq 1 120); do
    alive=0
    for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1;done
    [ "$alive" = 0 ] && break
    sleep 1
done
[ "$alive" = 0 ] || { echo 'daemons still draining' >&2;exit 1; }
EFS_BATCH_OBS=0 EFS_COMMANDS_SERVER=0 EFS_QUERY_CATALOG_MIB=1024 \
EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp \
    /data1/efs/devops/start.sh > "$base/final-start.log" 2>&1 || {
    EFS_BATCH_OBS=0 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp /data1/efs/devops/start.sh >> "$base/final-start.log" 2>&1;
}
mountpoint -q /data1/efs/mnt
mountpoint -q /data1/efs/mnt2
ps -C efsd -C efs-fuse -o pid,args > "$base/final-processes.txt"
echo 'cluster ready, batching diagnostics disabled'
