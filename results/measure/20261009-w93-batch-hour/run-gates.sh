#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-batch-hour-20261009
build=/data1/efs/w97-flow-build
bash "$base/deploy.sh"
python3 "$build/tests/posix/posix_suite.py" /data1/efs/mnt --jobs 4 --timeout-s 30 --results "$base/posix.tsv" > "$base/posix.log" 2>&1
python3 "$build/tests/posix/posix_2client.py" --local /data1/efs/mnt /data1/efs/mnt2 --parent w93-batch-hour-peer --results "$base/peer.tsv" > "$base/peer.log" 2>&1
printf '0\n' > "$base/live-gates-exit.txt"
echo enhanced-functional-gates-complete
