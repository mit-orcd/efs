#!/bin/bash
set -euo pipefail
root=/data1/efs/bench/w92-scale-2p32-20261010
n=${1:?actual entry count required}
finish() { status=$?; printf '%s\n' "$status" > "$root/exit-$n"; touch "$root/finished-$n"; }
trap finish EXIT
export EFS_QUERY_DISK_RESERVE_MIB=102400
export EFS_QUERY_DISK_MAX_MIB=8388608
export EFS_QUERY_DISK_TOTAL_MIB=16777216
/usr/bin/time -v -o "$root/scale-$n.time" /home/efs/w92-query-scale-sequential "$root/scale-$n" "$n" > "$root/scale-$n.log" 2>&1
du -sb "$root/scale-$n" > "$root/scale-$n.logical"
du -s "$root/scale-$n" > "$root/scale-$n.allocated"
df -h "$root" /data1 > "$root/scale-$n.space"
