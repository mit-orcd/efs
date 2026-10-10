#!/usr/bin/env bash
set -euo pipefail
base=/mnt/w92-scale-big-20261010
stem=scale-4294967296-raw-batched-v6
fixture=$base/scale-4294967296-bounded-24g-v4
ulimit -n 524288
ulimit -c 0
printf '700\n' > /proc/$$/oom_score_adj
export EFS_QUERY_DISK_RESERVE_MIB=102400
export EFS_QUERY_DISK_TOTAL_MIB=16777216
export EFS_QUERY_DISK_MAX_MIB=8388608
set +e
nice -n 19 ionice -c 3 /usr/bin/time -v -o "$base/$stem.time" \
  /home/efs/w92-query-scale-raw-batched "$fixture" 4294967296 --resume-build \
  > "$base/$stem.log" 2>&1 &
w92_job_pid=$!
printf '%s\n' "$w92_job_pid" > "$base/$stem.pid"
wait "$w92_job_pid"
w92_job_rc=$?
printf '%s\n' "$w92_job_rc" > "$base/exit-4294967296-batched-v5"
date -u > "$base/finished-4294967296-batched-v5"
exit "$w92_job_rc"
