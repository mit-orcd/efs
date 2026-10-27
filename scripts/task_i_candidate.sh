#!/usr/bin/env bash
# Task I: qualify an integrated candidate's full code gate before its hour.
# Usage: task_i_candidate.sh COMMIT NEW_EVIDENCE_ROOT
set -u
commit=${1:?exact source commit required}
art=${2:?fresh external evidence root required}
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$(pwd -P)" == "$HOME"/efs-taskI/*/src ]] || exit 2
mkdir "$art" || exit 2
mkdir "$art/gate"
exec >"$art/controller.log" 2>&1
finish() {
 rc=$?
 if [ "$(cat "$art/stage" 2>/dev/null)" != FINISHED ]; then
  printf 'INCOMPLETE\n' > "$art/incomplete"
  [ "$rc" -ne 0 ] || rc=1
 fi
 printf '%s\n' "$rc" > "$art/controller-exit"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
export EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit" EFS_GIT_BRANCH=agent/devel-task-i
export EFS_FUSE_PREFIX=/usr EFS_TRANSPORT=tcp
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
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$f" ] || cp "$f" "$art/gate/$f"
done
for f in tmp/fail-*.log; do [ ! -f "$f" ] || cp "$f" "$art/gate/"; done
./efsd --version > "$art/build-identity.txt" 2>&1
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt > "$art/binaries.sha256"
# Exit 3 is explicit prerequisite-limited coverage, never an absolute PASS.
# Keep that receipt even when the independent hour subsequently passes.
[ "$rc" -eq 0 ] || [ "$rc" -eq 3 ] || { echo FINISHED > "$art/stage"; exit "$rc"; }
printf 'MEASUREMENT\n' > "$art/stage"
bash scripts/task_i_run.sh "$commit" "$art/hour" tcp 127.0.0.1 measure-only
hour_rc=$?
echo "$hour_rc" > "$art/hour-exit"
echo FINISHED > "$art/stage"
[ "$hour_rc" -eq 0 ] || exit "$hour_rc"
exit "$rc"
