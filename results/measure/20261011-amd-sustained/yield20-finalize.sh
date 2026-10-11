#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-gc-yield20-20261011
while [ ! -f "$out/resume.exit" ]; do sleep 15; done
[ "$(cat "$out/live/restore.exit")" = 0 ]
trap 'echo $? > "$out/canonical-deploy.exit"' EXIT
python3 "$out/align-amd-profile.py"
EFS_AMD_BUILD_ID=715b1103 python3 "$out/deploy-yield20.py" > "$out/canonical-deploy.log" 2>&1
python3 "$out/restoration-proof.py" >> "$out/canonical-deploy.log" 2>&1
