#!/usr/bin/env bash
# Untouched-base comparison after a failed Task I gate. Run on quiet nuc.
# Usage: bash /path/task_i_base_gate.sh BASE_COMMIT NEW_ARTIFACTS
set -u
commit=${1:?base commit required}
art=${2:?fresh external evidence directory required}
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$(pwd -P)" == "$HOME/efs-taskI/base/src" ]] || exit 2
mkdir "$art" || exit 2
mkdir "$art/gate"
exec >"$art/controller.log" 2>&1
trap 'rc=$?; printf "%s\n" "$rc" > "$art/controller-exit"' EXIT
export EFS_FUSE_PREFIX=/usr EFS_TRANSPORT=tcp
export EFS_GIT_ID="$commit" EFS_GIT_BRANCH=devel EFS_GIT_DESCRIBE="$commit"
python3 - <<'PY'
import sys
sys.path.insert(0,'scripts')
from soak import foreign_jobs
jobs=foreign_jobs()
if jobs:raise SystemExit('NOTRUN: quiet host required: '+repr(jobs))
PY
[ "$?" -eq 0 ] || exit 77
printf 'GATE\n' > "$art/stage"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
rc=$?
printf '%s\n' "$rc" > "$art/gate/exit"
for name in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$name" ] || cp "$name" "$art/gate/$name"
done
for name in tmp/fail-*.log; do [ ! -f "$name" ] || cp "$name" "$art/gate/"; done
./efsd --version > "$art/build-identity.txt" 2>&1
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt > "$art/binaries.sha256"
printf 'FINISHED\n' > "$art/stage"
exit "$rc"
