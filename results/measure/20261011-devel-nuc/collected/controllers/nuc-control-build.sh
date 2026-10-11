#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
while [ ! -f "$base/fixed-v2-build/native-exit.txt" ]; do sleep 2; done
[ "$(cat "$base/fixed-v2-build/native-exit.txt")" = 0 ] || exit 1
[ ! -e "$base/control" ] || exit 1
cp -a --reflink=auto "$base/candidate" "$base/control" || exit 1
cp "$base/report-control.c" "$base/control/src/server/raft_host.c" || exit 1
mkdir "$base/control-build" || exit 1
cd "$base/control" || exit 1
make -j4 efsd EFS_GIT_ID=2669064e-sync-control EFS_GIT_DESCRIBE=2669064e-sync-control EFS_GIT_BRANCH=devel-nuc >"$base/control-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/control-build/build-exit.txt"; exit "$rc"
