#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
while [ ! -f "$base/fixed-v3-build/native-exit.txt" ]; do sleep 2; done
[ "$(cat "$base/fixed-v3-build/native-exit.txt")" = 0 ] || exit 1
cp "$base/nuc-report-control-v3.c" "$base/control/src/server/raft_host.c" || exit 1
mkdir "$base/control-v3-build" || exit 1
cd "$base/control" || exit 1
make -j4 efsd EFS_GIT_ID=108afa3e-sync-control EFS_GIT_DESCRIBE=108afa3e-sync-control EFS_GIT_BRANCH=devel-nuc >"$base/control-v3-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/control-v3-build/build-exit.txt"; exit "$rc"
