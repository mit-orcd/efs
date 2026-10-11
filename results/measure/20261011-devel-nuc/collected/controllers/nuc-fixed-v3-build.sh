#!/bin/bash
set -u
base=/data1/efs/devel-nuc-20261011
cp "$base/raft_host.c" "$base/candidate/src/server/raft_host.c"
cp "$base/test_txn_apply_retry.py" "$base/candidate/tests/test_txn_apply_retry.py"
cd "$base/candidate" || exit 1
mkdir -p "$base/fixed-v3-build"
make -j4 all EFS_GIT_ID=108afa3e-nuc EFS_GIT_DESCRIBE=108afa3e-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-v3-build/build.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-v3-build/build-exit.txt"
[ "$rc" = 0 ] || exit "$rc"
make -j4 test EFS_GIT_ID=108afa3e-nuc EFS_GIT_DESCRIBE=108afa3e-nuc EFS_GIT_BRANCH=devel-nuc >"$base/fixed-v3-build/native.log" 2>&1
rc=$?; echo "$rc" >"$base/fixed-v3-build/native-exit.txt"; exit "$rc"
