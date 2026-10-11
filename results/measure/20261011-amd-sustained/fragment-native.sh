#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-gc-fragments-20261011
out=/data1/efs/amd-gc-fragments-20261011
build=${EFS_AMD_BUILD_ID:?set the captured commit ID}
[[ $build =~ ^[0-9a-f]{8,40}$ ]]
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
trap 'echo $? > "$out/native-round.exit"' EXIT
cd "$src"
for mode in normal fault; do
    faults=0
    [ "$mode" != fault ] || faults=1
    make clean > "$out/$mode-clean.log" 2>&1
    set +e
    make -j8 EFS_FAULTS="$faults" EFS_GIT_ID="$build-final-$mode" EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE="$build-final-$mode" all test > "$out/$mode-native.log" 2>&1
    rc=$?
    set -e
    echo "$rc" > "$out/$mode-native.exit"
    [ "$rc" = 0 ] || exit "$rc"
    ./efsd --version > "$out/$mode-build.txt"
    sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/$mode-binary-sha256.txt"
done
