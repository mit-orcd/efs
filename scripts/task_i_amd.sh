#!/usr/bin/env bash
# User-authorized AMD: wait for all-account project work, then native RDMA
# tests, full TCP gate and the unchanged F1 hour. rxe0 is software RDMA;
# node9900's mlx5 run remains the hardware qualification.
# COMMIT NEW_EVIDENCE_ROOT
set -u
commit=${1:?exact source commit required}
art=${2:?new evidence root required}
[[ "$(hostname -s)" == amd-* && "$(pwd -P)" == "$HOME"/efs-taskI/*/src ]] || exit 2
[[ "$commit" =~ ^[0-9a-f]{9,40}$ ]] || exit 2
mkdir "$art" || exit 2
mkdir "$art/rdma" "$art/gate"
exec >"$art/controller.log" 2>&1
finish() {
 rc=$?
 if [ "$(cat "$art/stage" 2>/dev/null)" != FINISHED ]; then
  echo INCOMPLETE > "$art/incomplete";[ "$rc" -ne 0 ] || rc=1
 fi
 echo "$rc" > "$art/controller-exit"
}
trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
export EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit" EFS_GIT_BRANCH=agent/devel-task-i
export EFS_FUSE_PREFIX=/usr EFS_RDMA_DEV=rxe0
export PATH="$HOME/bin:$PATH"
unset LD_PRELOAD TASK_I_ALLOC_DIR MALLOC_ARENA_MAX
echo WAIT_QUIET > "$art/stage"
end=$((SECONDS+14400));quiet=0
while [ "$SECONDS" -lt "$end" ]; do
 python3 - "$art" <<'PY'
import json,sys,time
from pathlib import Path
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
(Path(sys.argv[1])/'last-preflight.json').write_text(json.dumps({'epoch':time.time(),'jobs':jobs},indent=2)+'\n')
raise SystemExit(1 if jobs else 0)
PY
 if [ "$?" -eq 0 ]; then quiet=$((quiet+1));else quiet=0;fi
 [ "$quiet" -lt 2 ] || break
 sleep 20
done
[ "$quiet" -ge 2 ] || exit 124
{ hostname; date -u; uname -a; id; free -h; df -h "$HOME"; gcc --version; pkg-config --modversion fuse3; ibv_devinfo -d rxe0; ulimit -l; } > "$art/environment.txt" 2>&1
echo BUILD > "$art/stage"
make -j8 all tests/faults/efsd test-bins > "$art/build.log" 2>&1
rc=$?;echo "$rc" > "$art/build-exit";[ "$rc" -eq 0 ] || exit "$rc"
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt tests/test_rdma_xprt tests/test_rdma_segmented tests/test_declaration_gate > "$art/binaries-before.sha256"
echo RDMA_UNITS > "$art/stage"
status=0
run_rdma() {
 name=$1;shift
 EFS_TRANSPORT=rdma timeout --signal=TERM --kill-after=10s 300 "$@" > "$art/rdma/$name.log" 2>&1
 rc=$?;echo "$rc" > "$art/rdma/$name-exit"
 if [ "$rc" -ne 0 ]; then status=1;elif [ "$status" -eq 0 ] && grep -qE 'NOTRUN|SKIP' "$art/rdma/$name.log"; then status=3;fi
}
run_rdma declaration ./tests/test_declaration_gate
run_rdma xprt ./tests/test_rdma_xprt
for mode in normal interrupt tcp-violation deadline burst burst-asym churn; do
 run_rdma "$mode" ./tests/test_rdma_segmented "$mode"
done
# A skip/failure is never hidden by the later full gate or measurement.
echo "$status" > "$art/rdma/exit"
[ "$status" -eq 0 ] || { echo FINISHED > "$art/stage";exit "$status"; }
echo GATE > "$art/stage"
EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
rc=$?;echo "$rc" > "$art/gate/exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$f" ] || cp "$f" "$art/gate/$f"
done
for f in tmp/fail-*.log; do [ ! -f "$f" ] || cp "$f" "$art/gate/"; done
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt > "$art/binaries-after.sha256"
[ "$rc" -eq 0 ] || [ "$rc" -eq 3 ] || { echo FINISHED > "$art/stage";exit "$rc"; }
echo MEASUREMENT > "$art/stage"
bash scripts/task_i_run.sh "$commit" "$art/hour" tcp 127.0.0.1 measure-only
hour_rc=$?;echo "$hour_rc" > "$art/hour-exit"
echo FINISHED > "$art/stage"
[ "$hour_rc" -eq 0 ] || exit "$hour_rc"
exit "$rc"
