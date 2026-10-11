#!/bin/bash
set -euo pipefail
previous=/data1/efs/amd-gc-fanout-20261011/long
out=/data1/efs/amd-gc-yield20-20261011
while [ ! -f "$previous/round.exit" ] || [ ! -f "$previous/restore.exit" ]; do sleep 15; done
[ "$(cat "$previous/restore.exit")" = 0 ]
EFS_AMD_BUILD_ID=715b1103 bash "$out/yield20-native.sh"
EFS_AMD_BUILD_ID=715b1103 bash "$out/yield20-resume.sh"
