#!/bin/bash
set -euo pipefail
fixture_root=/data1/efs/bench/w92-scale-volume-20261010
finish() { status=$?; printf '%s\n' "$status" > "$fixture_root/exit-code-10m-v5"; touch "$fixture_root/finished-10m-v5"; }
trap finish EXIT
export EFS_QUERY_DISK_RESERVE_MIB=1024
for n in 10000000; do
 /usr/bin/time -v -o "$fixture_root/scale-$n-v5.time" /home/efs/w92-query-scale-v5 "$fixture_root/fs/scale-$n-v5" "$n" > "$fixture_root/scale-$n-v5.log" 2>&1
 printf '0\n' > "$fixture_root/scale-$n-v5.exit"
 du -sb "$fixture_root/fs/scale-$n-v5" > "$fixture_root/scale-$n-v5.logical"
 du -s "$fixture_root/fs/scale-$n-v5" > "$fixture_root/scale-$n-v5.allocated"
 df -h "$fixture_root/fs" /data1 > "$fixture_root/scale-$n-v5.space"
 printf 'completed %s\n' "$n" >> "$fixture_root/progress-10m-v5.log"
done
