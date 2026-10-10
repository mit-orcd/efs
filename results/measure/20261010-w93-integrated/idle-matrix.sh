#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-integrated-20261010
cd "$base"
[ "$(cat comparison-exit.txt)" = 0 ]
python3 idle-check.py idle-control1 10 0 6000 reuse > idle-control1-controller.log 2>&1
python3 idle-check.py idle-cadence 100 0 6000 > idle-cadence-controller.log 2>&1
python3 idle-check.py idle-cohort 100 1 6000 > idle-cohort-controller.log 2>&1
python3 idle-check.py idle-control2 10 0 6000 > idle-control2-controller.log 2>&1
python3 balanced-verify.py > post-idle-cold-verification.log
printf '0\n' > idle-exit.txt
python3 final-state.py > final-state.log
