#!/usr/bin/env bash
# Queue one integrated candidate behind an existing Task I hardware pair.
# COMMIT NEW_EVIDENCE_ROOT EXISTING_CLIENT_PID PRIOR_EVIDENCE_ROOT
set -u
commit=${1:?source commit required}
art=${2:?fresh evidence root required}
client=${3:?existing client PID required}
prior=${4:?prior hardware evidence root required}
[[ "$(hostname -s)" == node9900 && "$(pwd -P)" == /tmp/efs-taskI-*/src ]] || exit 2
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$prior" == /data1/efs/evidence/taskI-* ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
finish() {
 rc=$?
 if [ "$(cat "$art/stage" 2>/dev/null)" != FINISHED ]; then
  echo INCOMPLETE > "$art/incomplete"
  [ "$rc" -ne 0 ] || rc=1
 fi
 echo "$rc" > "$art/controller-exit"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
echo WAIT_PRIOR_HARDWARE > "$art/stage"
end=$((SECONDS+14400))
until [ "$(cat "$prior/stage" 2>/dev/null)" = FINISHED ] && [ -f "$prior/controller-exit" ]; do
 [ "$SECONDS" -lt "$end" ] || { echo 'Prior hardware deadline; nothing launched'; exit 124; }
 sleep 20
done
# A failed measurement is preserved and may precede a different candidate;
# a missing/incomplete prerequisite or unsafe cleanup never starts new work.
python3 - "$prior" "$art" "$commit" <<'PY'
import json,sys
from pathlib import Path
p,out=map(Path,sys.argv[1:3]);rows={}
for t in ('rdma','tcp'):
 q=p/'qualification'/t
 assert (q/'stage').read_text().strip()=='FINISHED',t+' not complete'
 assert (q/'measurement/exit').exists(),t+' missing workload receipt'
 assert int((q/'controller-exit').read_text()) in (0,1),t+' prerequisite/cleanup failure'
 d=json.loads((q/'measurement/d4-result.json').read_text())
 rows[t]={'verdict':d['verdict'],'reason':d.get('reason','')}
(out/'prior-results.json').write_text(json.dumps({'prior':str(p),'candidate':sys.argv[3],'results':rows},indent=2)+'\n')
PY
[ "$?" -eq 0 ] || exit 77
unset LD_PRELOAD TASK_I_ALLOC_DIR MALLOC_ARENA_MAX
echo ALLOCATOR_UNIT > "$art/stage"
python3 tests/test_client_allocator.py > "$art/allocator-unit.log" 2>&1
rc=$?;echo "$rc" > "$art/allocator-unit-exit"
[ "$rc" -eq 0 ] || exit "$rc"
echo HARDWARE_QUALIFICATION > "$art/stage"
bash scripts/task_i_hardware.sh "$commit" "$art/qualification" "$client"
rc=$?;echo "$rc" > "$art/qualification-exit"
echo FINISHED > "$art/stage"
exit "$rc"
