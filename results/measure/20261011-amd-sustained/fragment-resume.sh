#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-gc-fragments-20261011
out=/data1/efs/amd-gc-fragments-20261011
build=${EFS_AMD_BUILD_ID:?set captured source revision}
export EFS_AMD_BUILD_ID
trap 'echo $? > "$out/resume.exit"' EXIT
while [ ! -f "$out/native-round.exit" ]; do sleep 15; done
[ "$(cat "$out/native-round.exit")" = 0 ]
cd "$src"
make clean > "$out/deploy-clean.log" 2>&1
make -j8 EFS_FAULTS=0 EFS_GIT_ID="$build-deploy-normal" EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE="$build-deploy-normal" all > "$out/deploy-build.log" 2>&1
python3 "$out/deploy-fragments.py" > "$out/deploy.log" 2>&1
echo 0 > "$out/deploy.exit"
bash "$out/fragment-live.sh"
