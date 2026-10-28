#!/usr/bin/env bash
# Reproduce the retained B6 slow failure on an untouched base export.
set -eu
src=${1:?untouched base source};prior=${2:?closed AF campaign};art=${3:?fresh evidence}
[[ $(hostname -s) == xorinox && $src == "$HOME/efs-task-af/base-486/src" ]] || exit 2
[[ $(cat "$prior/stage") == FINISHED ]] || exit 77
mkdir "$art";exec >"$art/run.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$src";mkdir -p tmp
export EFS_GIT_ID=486aa3a46 EFS_GIT_DESCRIBE=486aa3a46 EFS_GIT_BRANCH=agent/devel-task-af-base
export TMPDIR=$src/tmp EFS_TRANSPORT=tcp EFS_B6_ARTIFACTS=$art/measurement
echo BUILD > "$art/stage"
make -j8 efsd efs-fuse efs-mgmt tests/faults/efsd > "$art/build.log" 2>&1
{ ./efsd --version;sha256sum src/client/data_commit.c src/server/raft_host.c tests/test_b6_unavailable.py efs-fuse tests/faults/efsd; } > "$art/identity.txt"
echo B6_SLOW > "$art/stage"
EFS_B6_MODES=slow timeout --signal=TERM --kill-after=10s 900 python3 tests/test_b6_unavailable.py > "$art/test.log" 2>&1 || rc=$?
echo "${rc:-0}" > "$art/test-exit"
echo FINISHED > "$art/stage"
exit "${rc:-0}"
