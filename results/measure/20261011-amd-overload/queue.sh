#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-overload-20261011
src=/data1/efs/devel-amd-overload-20261011
while [ ! -f "$out/normal-native.exit" ]; do sleep 15; done
[ "$(cat "$out/normal-native.exit")" = 0 ]
trap 'echo $? > "$out/queue.exit"' EXIT
cd "$src"
make clean > "$out/fault-clean.log" 2>&1
set +e
make -j8 EFS_FAULTS=1 EFS_GIT_ID=54e33e82-admission-fault EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=admission-fault all test > "$out/fault-native.log" 2>&1
rc=$?
set -e
echo "$rc" > "$out/fault-native.exit"
[ "$rc" = 0 ]
make clean > "$out/live-clean.log" 2>&1
make -j8 EFS_GIT_ID=54e33e82-admission-normal EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=admission-normal all > "$out/live-build.log" 2>&1
./efsd --version > "$out/live-build.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/live-binary-sha256.txt"
bash "$out/live.sh"
