#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
cd "$base/candidate" || exit 1
mkdir -p "$base/fixed-v2-build"
make -j4 all EFS_GIT_ID=2669064e-nuc EFS_GIT_DESCRIBE=2669064e-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-v2-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-v2-build/build-exit.txt"
[ "$rc" = 0 ] || exit "$rc"
make -j4 test EFS_GIT_ID=2669064e-nuc EFS_GIT_DESCRIBE=2669064e-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-v2-build/native.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-v2-build/native-exit.txt"; exit "$rc"
