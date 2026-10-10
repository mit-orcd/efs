#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-closure-20261010
build=/data1/efs/w93-closure-build
[ -f "$base/gates-pass.txt" ]
[ "$(cat "$base/candidate-full100-exit.txt")" = 0 ]
cd "$build"
cc -O2 -g -Iinclude "$base/raft-cohort-cost.c" libefs.a -lpthread -lm -ldl -libverbs -o "$base/raft-cohort-cost" > "$base/cohort-build.log" 2>&1
sha256sum libefs.a "$base/raft-cohort-cost" "$base/raft-cohort-cost.c" > "$base/cohort-sha256.txt"
for target in /data1/efs /data2/efs; do
 label=$(basename "$(dirname "$target")")
 for repeat in 1 2; do
  for groups in 1 2; do
   for width in 1 16; do
    for batch in 0 1; do
     "$base/raft-cohort-cost" "$batch" "$groups" "$width" "$target" > "$base/cohort-$label-$repeat-$groups-$width-$batch.json"
    done
   done
  done
 done
done
printf '0\n' > "$base/cohort-exit.txt"
