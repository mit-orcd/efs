#!/bin/bash
set -euo pipefail
base=/data1/efs/w97-restored-20261009
while [ ! -f /data1/efs/w97-partial-reply-20261009/live-gates-exit.txt ]; do sleep 5; done
# All gates and their append workers must be gone before a graceful rollback.
if pgrep -f '^python3 /data1/efs/w97-partial-build/tests/posix/' >/dev/null; then echo 'gate workers retained'; exit 1; fi
bash /data1/efs/w97-restore.sh
rc=0
python3 /data1/efs/w97-partial-build/tests/posix/posix_suite.py /data1/efs/mnt --filter concurrent_appends --jobs 1 --timeout-s 30 --results "$base/append-control.tsv" > "$base/append-control.log" 2>&1 || rc=$?
printf '%s\n' "$rc" > "$base/append-control-exit.txt"
echo baseline-restored-and-append-control-finished
