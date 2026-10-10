#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-checkpoint-hour-20261010
cd "$base"
[ -f comparison-pass.txt ]
profile_rc=0
bash profile.sh > profile-controller.log 2>&1 || profile_rc=$?
printf '%s\n' "$profile_rc" > profile-exit.txt
fault_rc=0
bash fault-gates.sh > fault-controller.log 2>&1 || fault_rc=$?
printf '%s\n' "$fault_rc" > fault-controller-exit.txt
[ "$fault_rc" = 0 ]
python3 balanced-verify.py > cold-verification.log 2>&1
python3 final-state.py > final-state.log 2>&1
python3 balanced-analyze.py > comparison.txt
python3 resource-summary.py > resource-summary.txt
python3 disk-summary.py > disk-summary.txt
printf 'finish PASS (profile exit %s)\n' "$profile_rc" > finish-pass.txt
exit "$profile_rc"
