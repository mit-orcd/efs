#!/bin/bash
set -euo pipefail
root=/data1/efs/amd-round-20261010
out=$root/final-native
mkdir -p "$out"
while [ ! -f "$root/adopt-fault/round.exit" ]; do sleep 3; done
[ "$(cat "$root/adopt-fault/round.exit")" = 0 ]
[ "$(cat "$root/adopt-fault/restore.exit")" = 0 ]
src=/data1/efs/devel-amd-final
[ ! -e "$src" ]
cp -a /data1/efs/devel-amd-fault "$src"
cd "$src"
make clean > "$out/clean.log" 2>&1
set +e
make -j8 EFS_FAULTS=0 EFS_GIT_ID=718e2dbc-final-normal EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=718e2dbc-final-normal all test > "$out/native.log" 2>&1
rc=$?
set -e
echo "$rc" > "$out/native.exit"
./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
exit "$rc"
