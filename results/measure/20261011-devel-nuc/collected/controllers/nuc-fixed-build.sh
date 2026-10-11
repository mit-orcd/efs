#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
cd "$base/candidate" || exit 1
mkdir -p "$base/fixed-build"
make -j4 all EFS_GIT_ID=fb7ca893-nuc EFS_GIT_DESCRIBE=fb7ca893-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-build/build-exit.txt"
[ "$rc" = 0 ] || exit "$rc"
make -j4 test EFS_GIT_ID=fb7ca893-nuc EFS_GIT_DESCRIBE=fb7ca893-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-build/native.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-build/native-exit.txt"; exit "$rc"
