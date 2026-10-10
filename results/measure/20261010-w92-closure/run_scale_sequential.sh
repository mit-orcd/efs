#!/bin/bash
set -euo pipefail
root=/mnt/w92-scale-big-20261010
n=${1:?actual entry count required}
run=${EFS_SCALE_RUN_ID:-}
[[ "$run" =~ ^[a-zA-Z0-9_-]*$ ]] || exit 2
suffix=${run:+-$run}
ulimit -Sn 524288
ulimit -c 0
# Prefer this private generator over production services under memory pressure.
echo 700 > /proc/$$/oom_score_adj
finish() { status=$?; printf '%s\n' "$status" > "$root/exit-$n$suffix"; touch "$root/finished-$n$suffix"; }
trap finish EXIT
export EFS_QUERY_DISK_RESERVE_MIB=102400
export EFS_QUERY_DISK_MAX_MIB=8388608
export EFS_QUERY_DISK_TOTAL_MIB=16777216
/usr/bin/time -v -o "$root/scale-$n$suffix.time" /home/efs/w92-query-scale-sequential "$root/scale-$n$suffix" "$n" > "$root/scale-$n$suffix.log" 2>&1
du -sb "$root/scale-$n$suffix" > "$root/scale-$n$suffix.logical"
du -s "$root/scale-$n$suffix" > "$root/scale-$n$suffix.allocated"
df -h "$root" /data1 > "$root/scale-$n$suffix.space"
