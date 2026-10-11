#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
trap 'echo $? > "$out/extra-queue.exit"' EXIT
while [ ! -f "$out/pressure/round.exit" ]; do sleep 15; done
[ "$(cat "$out/pressure/round.exit")" = 0 ]
bash "$out/pressure-extra.sh"
