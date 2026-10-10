#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-integrated-20261010
build=/data1/efs/w93-integrated-build
[ -f "$base/gates-pass.txt" ]
[ "$(cat "$base/candidate-full100-exit.txt")" = 0 ]
# Also exercise maximum retained tails with the accepted overlap/no-op bundle.
export EFS_RAFT_FLOW=1 EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_GROUP_DRAIN=0 EFS_RAFT_BATCH_OVERLAP=1 EFS_COMMANDS_SERVER=0 EFS_COMMANDS=0 EFS_RAFT_APPLIED_INTERVAL_MS=1000
for spec in "candidate-kv1000:24390:--kv-wal-persistence-only" "candidate-marker1000:24490:--applied-checkpoint-replay-only"; do
 IFS=: read -r name port flag <<< "$spec"
 rc=0
 python3 "$build/tests/live/gc_reclamation.py" --mode direct --transport tcp --port "$port" --mount /data1/efs/w93-checkpoint-replay-mnt --peer-mount /data1/efs/w93-checkpoint-replay-mnt2 "$flag" > "$base/$name.log" 2>&1 || rc=$?
 printf '%s\n' "$rc" > "$base/$name-exit.txt"
 [ "$rc" = 0 ] || exit "$rc"
done
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
