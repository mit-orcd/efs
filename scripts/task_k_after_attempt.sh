#!/usr/bin/env bash
# Wait for our earlier attempt only; never stop an unrelated process or service.
# Usage: bash scripts/task_k_after_attempt.sh COMMIT ARTIFACTS_PARENT IPV4
set -u
commit=${1:?}; parent=${2:?}; address=${3:?}
old=/tmp/efs-taskK-20261026/candidate
oldart="$parent/qualification-d643adcd9"
end=$((SECONDS+14400))
while [ ! -f "$oldart/controller-exit" ]; do
 if [ "$SECONDS" -ge "$end" ]; then echo 'BLOCKED: earlier attempt still owns host'; exit 77; fi
 sleep 300
done
python3 - "$old" <<'PY'
from pathlib import Path
import os,sys
root=Path(sys.argv[1]);left=[]
for p in Path('/proc').glob('[0-9]*'):
 try:
  if p.stat().st_uid==os.getuid() and (p/'exe').resolve().is_relative_to(root):left.append(str(p))
 except (OSError,ValueError):pass
left.extend(x for x in Path('/proc/self/mountinfo').read_text().splitlines() if str(root)+'/' in x)
if left:raise SystemExit('BLOCKED: earlier fixture processes/mounts remain: '+repr(left))
PY
[ "$?" -eq 0 ] || exit 125
# Re-run the exact final regression against unchanged base product code.
# Only this measurement driver is added, then moved out before gate enumeration.
base=/tmp/efs-taskK-20261026/base
cp scripts/task_k_routing.py "$base/scripts/task_k_routing.py" || exit 2
TMPDIR="$base/tmp" EFS_TRANSPORT=tcp python3 -u "$base/scripts/task_k_routing.py" \
 --port-min 19201 --out "$parent/base/final-negative" > "$parent/base/final-negative.log" 2>&1
negative_rc=$?; echo "$negative_rc" > "$parent/base/final-negative-exit"
mv "$base/scripts/task_k_routing.py" "$parent/base/final-negative-driver.py" || exit 2
if [ "$negative_rc" -eq 0 ] || ! grep -q 'K44 retired-seed raft-mkfs:' "$parent/base/final-negative.log"; then
 echo 'BLOCKED: final negative control did not fail with K44; retain evidence and investigate'
 exit 1
fi
bash scripts/task_k_qualify.sh "$commit" "$parent/qualification-$commit" "$address"
rc=$?
# A failed candidate gate requires the untouched base on the same host/command.
summary="$parent/qualification-$commit/gate/summary.txt"
if [ -f "$summary" ] && head -n1 "$summary" | grep -Eq 'FAIL [1-9][0-9]*'; then
 base=/tmp/efs-taskK-20261026/base
 # This one added driver was the recorded negative control; move it out so
 # the base gate enumerates exactly the archived base tests.
 if [ -f "$base/tests/test_k44_retired_routing.py" ]; then
  mv "$base/tests/test_k44_retired_routing.py" "$parent/base/original-repro-driver.py"
 fi
 cd "$base" || exit 2
 mkdir "$parent/base/gate" || exit 2
 export EFS_GIT_ID=4066f74df EFS_GIT_BRANCH=agent/devel-task-k EFS_GIT_DESCRIBE=4066f74df
 export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_BUILD_JOBS=8 EFS_TRANSPORT=tcp
 EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$parent/base/gate/run.log" 2>&1
 base_rc=$?; echo "$base_rc" > "$parent/base/gate/exit"
 for file in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
  [ ! -f "$file" ] || cp "$file" "$parent/base/gate/$file"
 done
 for file in tmp/fail-*.log; do [ ! -f "$file" ] || cp "$file" "$parent/base/gate/"; done
fi
exit "$rc"
