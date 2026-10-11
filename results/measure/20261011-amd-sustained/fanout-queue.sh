#!/bin/bash
set -euo pipefail
previous=/data1/efs/amd-gc-fragments-20261011
out=/data1/efs/amd-gc-fanout-20261011
while [ ! -f "$previous/resume.exit" ]; do sleep 15; done
# Previous failure is retained; tests must finish and mounts restore first.
[ "$(cat "$previous/live/restore.exit")" = 0 ]
EFS_AMD_BUILD_ID=e9ea8ca1 bash "$out/fanout-native.sh"
EFS_AMD_BUILD_ID=e9ea8ca1 bash "$out/fanout-resume.sh"
