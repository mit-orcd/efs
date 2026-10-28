#!/usr/bin/env bash
# Assigned xorinox; no real cluster/store/client operation.
set -eu
root=${1:?private root};art=${2:?new evidence};commit=${3:?source identity}
[[ $(hostname -s) == xorinox && $root == "$HOME/efs-task-af" ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
mkdir "$root/lock" || exit 77
trap 'rc=$?;rmdir "$root/lock";echo "$rc" > "$art/controller-exit"' EXIT
{ date -u;hostname;uname -a;id;free -h;df -h "$root";ps -eo uid,pid,etime,args; } > "$art/environment.txt"
mkdir "$root/measure";cp -a "$root/candidate/src" "$root/measure/src"
echo INSTRUMENT_BUILD > "$art/stage"
python3 "$root/candidate/src/scripts/af_instrument.py" "$root/measure/src"
(cd "$root/measure/src";make -j8 EFS_GIT_ID="$commit-af" EFS_GIT_BRANCH=agent/devel-task-af all tests/faults/efsd) > "$art/build-measure.log" 2>&1
(cd "$root/measure/src";./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/server/raft_host.c) > "$art/measure-identity.txt"
n=0
# Latin square: each frequency occupies each position once.
for hz in 1 10 50 10 50 1 50 1 10;do
 n=$((n+1));echo "MEASURE_${n}_${hz}Hz" > "$art/stage"
 (cd "$root/measure/src";python3 -u scripts/af_measure.py --out "$art/trial-$n-${hz}hz" --hz "$hz" --seconds 60 --rate 10) > "$art/trial-$n-${hz}hz.log" 2>&1
done
echo BUILD_CLEAN > "$art/stage"
cd "$root/candidate/src"
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-task-af
make -j8 all tests/faults/efsd > "$art/build-clean.log" 2>&1
./efsd --version > "$art/clean-identity.txt"
sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd > "$art/binaries.sha256"
echo GATE > "$art/stage"
EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate.log" 2>&1 || gate_rc=$?
echo "${gate_rc:-0}" > "$art/gate-exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw;do [[ ! -f $f ]] || cp "$f" "$art/";done
for f in tmp/fail-*.log;do [[ ! -f $f ]] || cp "$f" "$art/";done
echo FINISHED > "$art/stage"
exit "${gate_rc:-0}"
