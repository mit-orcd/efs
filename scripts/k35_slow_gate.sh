#!/usr/bin/env bash
# Fresh private intel2 export. Product source is the explicit commit argument.
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?source commit}
[[ $(hostname -s) == intel2 && $src == "$HOME/efs-k35-"*/src ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$src"
{ date -u;hostname;uname -a;id;free -h;df -h "$src";ps -eo uid,pid,etime,args; } > "$art/environment.txt"
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-k35-slow-fsync
export EFS_TRANSPORT=tcp EFS_K35_KEEP=1 EFS_B6_KEEP=1
export EFS_B6_ARTIFACTS=$art/b6
echo GATE > "$art/stage"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate.log" 2>&1 || rc=$?
echo "${rc:-0}" > "$art/gate-exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw;do [[ ! -f $f ]] || cp "$f" "$art/";done
for f in tmp/fail-*.log;do [[ ! -f $f ]] || cp "$f" "$art/";done
{ ./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/client/data_commit.c; } > "$art/identity.txt"
echo FINISHED > "$art/stage"
exit "${rc:-0}"
