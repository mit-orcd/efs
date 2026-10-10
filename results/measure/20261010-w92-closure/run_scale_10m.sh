#!/bin/bash
set -euo pipefail
fixture_root=/data1/efs/bench/w92-scale-volume-20261010
finish() { status=$?; printf '%s\n' "$status" > "$fixture_root/exit-code-10m"; touch "$fixture_root/finished-10m"; }
trap finish EXIT
export EFS_QUERY_DISK_RESERVE_MIB=1024
for n in 10000000; do
 /usr/bin/time -v -o "$fixture_root/scale-$n.time" /home/efs/w92-query-scale-10m "$fixture_root/fs/scale-$n" "$n" > "$fixture_root/scale-$n.log" 2>&1
 printf '0\n' > "$fixture_root/scale-$n.exit"
 du -sb "$fixture_root/fs/scale-$n" > "$fixture_root/scale-$n.logical"
 du -s "$fixture_root/fs/scale-$n" > "$fixture_root/scale-$n.allocated"
 df -h "$fixture_root/fs" /data1 > "$fixture_root/scale-$n.space"
 printf 'completed %s\n' "$n" >> "$fixture_root/progress-10m.log"
done
