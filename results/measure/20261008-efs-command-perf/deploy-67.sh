#!/bin/bash
set -euo pipefail
round=$1
candidate=/data1/efs/command-build
installed=/data1/efs/src
output=/data1/efs/mnt/tiny-command-perf-r$round
backup=/data1/efs/command-perf/round$round/rollback
[ ! -e "$output" ]; [ ! -e "$backup" ]
generator=$(pgrep -f '^python3 ./tiny_files.py .* /data1/efs/mnt/(tiny-command-perf-final|tiny-command-perf-r[67])$' || true)
if [ -n "$generator" ]; then
 kill -TERM $generator
 for i in $(seq 1 30); do alive=0; for p in $generator; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
 [ "$alive" = 0 ] || exit 1
fi
"$installed/scripts/client.sh" stop /data1/efs/mnt
"$installed/scripts/client.sh" stop /data1/efs/mnt2
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)')
kill -TERM $pids
for i in $(seq 1 45); do alive=0; for p in $pids; do kill -0 "$p" 2>/dev/null && alive=1; done; [ "$alive" = 0 ] && break; sleep 1; done
[ "$alive" = 0 ] || exit 1
mkdir "$backup"
for b in efsd efs-fuse; do cp -p "$installed/$b" "$backup/$b"; cp -p "$candidate/$b" "$installed/$b.next"; mv "$installed/$b.next" "$installed/$b"; done
for d in src include tests; do rsync -a "$candidate/$d/" "$installed/$d/"; done
cp "$candidate/Makefile" "$installed/Makefile"
EFS_COMMANDS_SERVER=1 EFS_QUERY_CATALOG_MIB=1024 EFS_COMMANDS=1 EFS_CLUSTER=nuc EFS_DIRECT_IO=0 EFS_TRANSPORT=tcp /data1/efs/devops/start.sh
# Wait for initial publication, keeping all writes drained.
ready=0
for i in $(seq 1 60); do if /data1/efs/mnt/tiny-files4/.efs/stats --json > /data1/efs/command-perf/round$round/initial.json; then ready=1; break; fi; sleep 1; done
[ "$ready" = 1 ] || exit 1
screen -S 1110701.tiny_files -X stuff "cd /home/efs && ./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 1000000 --workers 8 $output"$'
'
