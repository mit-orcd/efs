#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
src=/data1/efs/devel-amd-feedback-20261011
trap 'echo $? > "$out/build-and-gates.exit"' EXIT
cd "$src"
make clean > "$out/fault-clean.log" 2>&1
make -j8 EFS_FAULTS=1 EFS_GIT_ID=amd-feedback-fault EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=amd-feedback-fault all test > "$out/fault-native.log" 2>&1
echo 0 > "$out/fault-native.exit"
mkdir -p "$out/fault-binaries"
cp efsd efs-fuse "$out/fault-binaries/"
sha256sum efsd efs-fuse > "$out/fault-binary-sha256.txt"
make clean > "$out/normal-clean.log" 2>&1
make -j8 EFS_GIT_ID=amd-feedback-normal EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=amd-feedback-normal all test > "$out/normal-native.log" 2>&1
echo 0 > "$out/normal-native.exit"
bash "$out/functional.sh"
