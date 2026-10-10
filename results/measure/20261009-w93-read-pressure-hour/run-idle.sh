#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-read-pressure-hour-20261009
cd "$base"
for item in off1:0 on1:1 off2:0; do
 tag=${item%:*};overlap=${item#*:};mkdir "$tag"
 EFS_RAFT_BATCH_NOOP=1 EFS_RAFT_BATCH_OVERLAP="$overlap" EFS_RAFT_GROUP_DRAIN=0 bash restart.sh 1 1 "$base/$tag" > "$base/$tag/controller.log" 2>&1
 sleep 10
 python3 read-capture.py "$tag" 90 > "$base/$tag/read.log" 2>&1
 echo "$tag PASS"
done
