#!/bin/bash
set -euo pipefail
candidate=/data1/efs/w93-closure-build
installed=/data1/efs/src
base=/data1/efs/w93-closure-20261010
mkdir -p "$base"
[ -f "$base/gates-pass.txt" ]
[ "$(cat "$base/default-full10-exit.txt")" = 0 ]
[ "$(cat "$base/cohort-exit.txt")" = 0 ]
[ "$(cat "$base/partition-exit.txt")" = 0 ]
[ "$(cat "$base/candidate-full100-exit.txt")" = 0 ]
# Never replace binaries beneath a live generator or a retained dirty mount.
if pgrep -f '^(python3|/usr/bin/python3) .*(tiny_files.py|loader-latency.py|balanced-load.py)' >/dev/null; then
    echo 'generator active; deployment refused' >&2; exit 1
fi
for mount in /data1/efs/mnt /data1/efs/mnt2; do
    # client.sh may race a successful exit after unmount and report that the
    # PID no longer identifies a mount. Require both safe postconditions.
    "$installed/scripts/client.sh" stop "$mount" || true
    if awk -v m="$mount" '$2==m {f=1} END {exit !f}' /proc/mounts || mountpoint -q "$mount" || pgrep -f "^[^ ]*efs-fuse .* $mount$" >/dev/null; then
        echo "client/mount retained: $mount; deployment refused" >&2; exit 1
    fi
done
if mountpoint -q /data1/efs/mnt || mountpoint -q /data1/efs/mnt2; then
    echo 'mount retained; deployment refused' >&2; exit 1
fi
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)' || true)
if [ -n "$pids" ]; then kill -TERM $pids; fi
alive=1
for i in $(seq 1 120); do
    alive=0
    for pid in $pids; do kill -0 "$pid" 2>/dev/null && alive=1; done
    [ "$alive" = 0 ] && break
    sleep 1
done
[ "$alive" = 0 ] || { echo 'daemons still draining; deployment refused' >&2; exit 1; }
backup="$base/rollback-before-5b2df4bd"
mkdir "$backup"
for binary in efsd efs-fuse efs-mgmt efs-query efs-bench; do
    [ ! -f "$installed/$binary" ] || cp -p "$installed/$binary" "$backup/$binary"
    cp -p "$candidate/$binary" "$installed/$binary.next"
    mv "$installed/$binary.next" "$installed/$binary"
done
for dir in src include scripts; do rsync -a "$candidate/$dir/" "$installed/$dir/"; done
cp "$candidate/Makefile" "$installed/Makefile"
"$installed/efsd" --version > "$base/deployed-fixed-version.txt"
sha256sum "$installed/efsd" "$installed/efs-fuse" > "$base/deployed-fixed-sha256.txt"
EFS_RAFT_APPLIED_INTERVAL_MS=10 EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_OVERLAP=0 EFS_RAFT_BATCH_NOOP=0 EFS_RAFT_GROUP_DRAIN=0 EFS_BATCH_OBS=1 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_CLUSTER=nuc EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp bash "$base/start-preserved.sh" > "$base/start.log" 2>&1
mountpoint -q /data1/efs/mnt
mountpoint -q /data1/efs/mnt2
echo deployment-complete
