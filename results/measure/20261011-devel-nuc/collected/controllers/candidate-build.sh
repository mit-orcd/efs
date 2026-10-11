#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
cd "$base/candidate" || exit 1
mkdir -p "$base/candidate-build"
make -j4 all EFS_GIT_ID=f2a0094b-nuc EFS_GIT_DESCRIBE=f2a0094b-nuc EFS_GIT_BRANCH=devel-nuc >"$base/candidate-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/candidate-build/build-exit.txt"
[ "$rc" = 0 ] || exit "$rc"
make -j4 test EFS_GIT_ID=f2a0094b-nuc EFS_GIT_DESCRIBE=f2a0094b-nuc EFS_GIT_BRANCH=devel-nuc >"$base/candidate-build/native.log" 2>&1
rc=$?; echo "$rc" >"$base/candidate-build/native-exit.txt"; exit "$rc"
