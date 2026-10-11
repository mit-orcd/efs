#!/bin/bash
set -euo pipefail
root=/data1/efs/amd-round-20261010
out=$root/final-native
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
trap 'echo $? > "$out/round.exit"' EXIT
# Native regression is independent of live fixture acceptance. Require restored
# service, but preserve a failed live verdict rather than suppress this build.
[ "$(cat "$root/adopt-fault/restore.exit")" = 0 ]
src=/data1/efs/devel-amd-final
[ ! -e "$src" ]
cp -a /data1/efs/devel-amd-fault "$src"
cd "$src"
make clean > "$out/clean.log" 2>&1
set +e
make -j8 EFS_FAULTS=0 EFS_GIT_ID=9c7c7a28-final-normal EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=9c7c7a28-final-normal all test > "$out/native.log" 2>&1
rc=$?
set -e
echo "$rc" > "$out/native.exit"
./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
exit "$rc"
