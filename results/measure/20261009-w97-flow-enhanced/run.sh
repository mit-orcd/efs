#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-flow-enhanced-20261009
build=/data1/efs/w97-flow-build
bash "$base/deploy.sh"
python3 "$base/capture.py"
python3 "$base/analyze.py" > "$base/analysis.txt"
python3 "$build/tests/posix/posix_suite.py" /data1/efs/mnt --jobs 4 --timeout-s 30 --results "$base/posix.tsv" > "$base/posix.log" 2>&1
python3 "$build/tests/posix/posix_2client.py" --local /data1/efs/mnt /data1/efs/mnt2 --parent w97-flow-enhanced-peer --results "$base/peer.tsv" > "$base/peer.log" 2>&1
bash "$base/finish.sh"
