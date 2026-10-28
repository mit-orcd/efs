#!/usr/bin/env bash
# Supplemental exact-rate50Hz confirmation, only after the clean gate closes.
set -eu
root=${1:?private root};prior=${2:?closed campaign};art=${3:?fresh evidence};driver_commit=${4:?driver commit}
[[ $(hostname -s) == xorinox && $root == "$HOME/efs-task-af/run-3" ]] || exit 2
[[ $(cat "$prior/stage") == FINISHED ]] || exit 77
gate=$(cat "$prior/gate-exit");[[ $gate == 0 || $gate == 1 || $gate == 3 ]] || exit 77
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
echo "$gate" > "$art/prior-gate-exit" # diagnostic does not override a failed gate
mkdir "$root/confirm"
cp -a "$root/measure/src" "$root/confirm/src"
cp "$(dirname "$0")/af_measure.py" "$root/confirm/src/scripts/af_measure.py"
{ echo "driver=$driver_commit; product/probe=f570fb0cd";cd "$root/confirm/src";./efsd --version;sha256sum tests/faults/efsd efs-fuse efs-mgmt src/server/raft_host.c scripts/af_measure.py; } > "$art/identity.txt"
for n in 1 2 3;do
 echo "MEASURE_${n}_50Hz" > "$art/stage"
 (cd "$root/confirm/src";python3 -u scripts/af_measure.py --out "$art/trial-$n-50hz" --hz 50 --seconds 60 --rate 5 --catch-up-polls) > "$art/trial-$n-50hz.log" 2>&1
done
echo FINISHED > "$art/stage"
