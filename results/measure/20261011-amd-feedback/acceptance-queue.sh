#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
trap 'echo $? > "$out/acceptance-queue.exit"' EXIT
while [ ! -f "$out/functional/round.exit" ]; do sleep 15; done
[ "$(cat "$out/functional/round.exit")" = 0 ]
bash "$out/lifecycle-fault.sh"
bash "$out/pressure.sh"
