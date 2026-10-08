#!/bin/bash
set -uo pipefail
run=/data1/efs/bench/w90-focused-20261008
cd /data1/efs/bench/w90-long-20261008T143207Z/src || exit 1
date -u +%FT%TZ > "$run/started"
./efs-bench.sh --storage-churn --compare \
  --storage-root "$run/target-a" --storage-root "$run/target-b" \
  --output "$run/results" --layouts files,punch \
  --payload-sizes 65536 --occupancies 95 --qds 4 \
  --io-modes direct --persist batch --batch-size 32 \
  --capacity-per-worker 2147483648 --container-bytes 1073741824 \
  --seconds 180 --repeat 4 --warmup 1000 --timeout 900 \
  --seed 42 --hot-set-percent 10 --hot-access-percent 90 \
  > "$run/progress.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$run/exit-code"
date -u +%FT%TZ > "$run/finished"
exit "$rc"
