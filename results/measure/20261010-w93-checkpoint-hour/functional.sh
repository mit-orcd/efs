#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-checkpoint-hour-20261010
build=/data1/efs/w93-checkpoint-build
python3 "$build/tests/posix/posix_suite.py" /data1/efs/mnt --jobs 4 --timeout-s 30 --results "$base/posix.tsv" > "$base/posix.log" 2>&1
python3 "$build/tests/posix/posix_2client.py" --local /data1/efs/mnt /data1/efs/mnt2 --parent w93-checkpoint-peer-20261010 --results "$base/peer.tsv" > "$base/peer.log" 2>&1
printf 'functional PASS\n' > "$base/functional-pass.txt"
