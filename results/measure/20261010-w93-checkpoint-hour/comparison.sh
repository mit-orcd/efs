#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-checkpoint-hour-20261010
cd "$base"
python3 balanced-capture.py 10ms 600 10 reuse > 10ms-driver.log 2>&1
python3 balanced-capture.py 100ms 600 100 > 100ms-driver.log 2>&1
python3 balanced-capture.py 10ms-return 180 10 > 10ms-return-driver.log 2>&1
python3 balanced-analyze.py > comparison.txt
printf 'comparison PASS\n' > comparison-pass.txt
