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
 workload_exit=int((q/'measurement/exit').read_text())
 controller_exit=int((q/'controller-exit').read_text())
 rows[t]={'driver_verdict':d['verdict'],'reason':d.get('reason',''),
          'workload_exit':workload_exit,'controller_exit':controller_exit,
          'overall_verdict':'PASS' if d['verdict']=='PASS' and workload_exit==0 and controller_exit==0 else 'FAIL'}
(out/'prior-results.json').write_text(json.dumps({'prior':str(p),'candidate':sys.argv[3],'results':rows},indent=2)+'\n')
PY
[ "$?" -eq 0 ] || exit 77
unset LD_PRELOAD TASK_I_ALLOC_DIR MALLOC_ARENA_MAX
export EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit" EFS_GIT_BRANCH=agent/devel-task-i
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_RDMA_DEV=mlx5_0
# A newer product needs its own full gate before the hours. The only
# permitted existing client is the same pinned orcd-mini identity used by
# task_i_run; this remains shared-client-host evidence, never quiet-host.
python3 - "$client" "$art" <<'PY'
import json,sys
from pathlib import Path
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs,background_identity
allowed=background_identity(sys.argv[1])
(Path(sys.argv[2])/'background-client.json').write_text(json.dumps(allowed,indent=2)+'\n')
jobs=[r for r in foreign_jobs() if r['pid']!=allowed['pid']]
if jobs:raise SystemExit('NOTRUN: other host work: '+repr(jobs))
PY
[ "$?" -eq 0 ] || exit 77
echo GATE > "$art/stage"
mkdir "$art/gate"
EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 \
 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
gate_rc=$?;echo "$gate_rc" > "$art/gate/exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$f" ] || cp "$f" "$art/gate/$f"
done
for f in tmp/fail-*.log; do [ ! -f "$f" ] || cp "$f" "$art/gate/"; done
[ "$gate_rc" -eq 0 ] || [ "$gate_rc" -eq 3 ] || { echo FINISHED > "$art/stage";exit "$gate_rc"; }
echo RDMA_UNITS > "$art/stage"
mkdir "$art/rdma-units"
native_rc=0
run_rdma() {
 name=$1;shift
 EFS_TRANSPORT=rdma timeout --signal=TERM --kill-after=10s 300 "$@" > "$art/rdma-units/$name.log" 2>&1
 rc=$?;echo "$rc" > "$art/rdma-units/$name-exit"
 if [ "$rc" -ne 0 ]; then native_rc=1;elif [ "$native_rc" -eq 0 ] && grep -qE 'NOTRUN|SKIP' "$art/rdma-units/$name.log";then native_rc=3;fi
}
run_rdma declaration ./tests/test_declaration_gate
run_rdma xprt ./tests/test_rdma_xprt
for mode in normal interrupt tcp-violation deadline burst burst-asym churn;do
 run_rdma "$mode" ./tests/test_rdma_segmented "$mode"
done
echo "$native_rc" > "$art/rdma-units/exit"
[ "$native_rc" -eq 0 ] || { echo FINISHED > "$art/stage";exit "$native_rc"; }
echo ALLOCATOR_UNIT > "$art/stage"
python3 tests/test_client_allocator.py > "$art/allocator-unit.log" 2>&1
rc=$?;echo "$rc" > "$art/allocator-unit-exit"
[ "$rc" -eq 0 ] || exit "$rc"
echo HARDWARE_QUALIFICATION > "$art/stage"
bash scripts/task_i_hardware.sh "$commit" "$art/qualification" "$client"
rc=$?;echo "$rc" > "$art/qualification-exit"
echo FINISHED > "$art/stage"
[ "$rc" -eq 0 ] || exit "$rc"
exit "$gate_rc"
