#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
cd "$base/src" || exit 1
mkdir -p "$base/build"
make -j6 all EFS_GIT_ID=c3118143-nuc EFS_GIT_DESCRIBE=c3118143-nuc EFS_GIT_BRANCH=devel-nuc >"$base/build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/build/build-exit.txt"
[ "$rc" = 0 ] || exit "$rc"
make -j6 test EFS_GIT_ID=c3118143-nuc EFS_GIT_DESCRIBE=c3118143-nuc EFS_GIT_BRANCH=devel-nuc >"$base/build/native.log" 2>&1
rc=$?; echo "$rc" >"$base/build/native-exit.txt"; exit "$rc"
