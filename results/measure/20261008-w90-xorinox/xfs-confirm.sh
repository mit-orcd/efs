#!/bin/bash
set -euo pipefail
root=/data1/efs/bench/w90-20261008
cd "$root/src"
for i in $(seq 1 180); do
  if [ -f "$root/legacy-principles.log" ] && grep -q '^OK$' "$root/legacy-principles.log"; then break; fi
  sleep 5
  if [ "$i" = 180 ]; then exit 1; fi
done
for round in 2 3; do
 for mode in baseline filestreams; do
  flags=()
  if [ "$mode" = filestreams ]; then flags=(--filestreams); fi
  name="xfs-confirm-$mode-$round"
  ./efs-bench.sh --storage-churn --storage-root "$root/scratch" --output "$root/$name" --io-modes direct --steps 24000 --layouts punch --occupancies 95 "${flags[@]}" > "$root/$name.log" 2>&1
  tail -2 "$root/$name.log"
 done
done
printf 'XFS_CONFIRM_PASS\n'
