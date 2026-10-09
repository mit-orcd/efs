#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-partial-reply-20261009
while [ ! -f /data1/efs/w97-clean-baseline-20261009/complete.json ]; do sleep 5; done
bash "$base/deploy.sh"
python3 "$base/capture.py"
rc=0
python3 /data1/efs/w97-partial-build/tests/posix/posix_suite.py /data1/efs/mnt --jobs 4 --timeout-s 30 --results "$base/posix.tsv" > "$base/posix.log" 2>&1 || rc=1
python3 /data1/efs/w97-partial-build/tests/posix/posix_2client.py --local /data1/efs/mnt /data1/efs/mnt2 --parent w97-partial-peer --results "$base/peer.tsv" > "$base/peer.log" 2>&1 || rc=1
printf '%s\n' "$rc" > "$base/live-gates-exit.txt"
# Preserve failed-gate evidence and require operator review before promotion.
if [ "$rc" != 0 ]; then echo 'Live gate failure: candidate retained for investigation; no acceptance claimed'; exit 1; fi
bash "$base/finish.sh"
echo candidate-measurement-and-gates-complete
