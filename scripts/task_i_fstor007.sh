#!/usr/bin/env bash
# User-selected quiet fstor007: full code gate, native RDMA, unchanged hours.
# COMMIT FRESH_EVIDENCE_ROOT; no existing client/cluster exception.
set -u
commit=${1:?exact exported commit}
art=${2:?fresh evidence root}
[[ $(hostname -s) == fstor007-mgmt && $(pwd -P) == /tmp/efs-taskI-fstor007-*/src ]] || exit 2
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$art" == /data1/efs/evidence/taskI-fstor007-* ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
export EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit" EFS_GIT_BRANCH=agent/devel-task-i
export EFS_FUSE_PREFIX=/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse
export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib/pkgconfig" LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib"
export EFS_RDMA_DEV=mlx5_2
unset LD_PRELOAD TASK_I_ALLOC_DIR MALLOC_ARENA_MAX EFS_QUERY_STAGE_TRACE EFS_RDMA_GID_INDEX
echo PREFLIGHT > "$art/stage"
for pass in 1 2; do
 python3 - <<'PY'
import sys
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
if jobs:raise SystemExit('NOTRUN: quiet host required: '+repr(jobs))
PY
 [ "$?" -eq 0 ] || exit 77
 [ "$pass" != 1 ] || sleep 20
done
[[ $(pkg-config --modversion fuse3) == 3.16.2 ]] || exit 77
{ date -u; hostname; uname -a; id; free -h; df -h /tmp; pkg-config --modversion fuse3;
  ip -4 -o addr show; ibv_devinfo -d mlx5_2; } > "$art/environment.txt" 2>&1
echo GATE > "$art/stage"
mkdir "$art/gate" "$art/rdma-units"
EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 \
 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
gate_rc=$?;echo "$gate_rc" > "$art/gate/exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$f" ] || cp "$f" "$art/gate/$f"
done
for f in tmp/fail-*.log; do [ ! -f "$f" ] || cp "$f" "$art/gate/"; done
if [ "$gate_rc" -ne 0 ] && [ "$gate_rc" -ne 3 ]; then echo FINISHED > "$art/stage";exit "$gate_rc";fi
echo RDMA_UNITS > "$art/stage"
native_rc=0
run_rdma() {
 name=$1;shift
 EFS_TRANSPORT=rdma timeout --signal=TERM --kill-after=10s 300 "$@" > "$art/rdma-units/$name.log" 2>&1
 rc=$?;echo "$rc" > "$art/rdma-units/$name-exit"
 if [ "$rc" -ne 0 ];then native_rc=1;elif [ "$native_rc" -eq 0 ] && grep -qE 'NOTRUN|SKIP' "$art/rdma-units/$name.log";then native_rc=3;fi
}
run_rdma declaration ./tests/test_declaration_gate
run_rdma xprt ./tests/test_rdma_xprt
for mode in normal interrupt tcp-violation deadline burst burst-asym churn;do
 run_rdma "$mode" ./tests/test_rdma_segmented "$mode"
done
echo "$native_rc" > "$art/rdma-units/exit"
if [ "$native_rc" -ne 0 ];then echo FINISHED > "$art/stage";exit "$native_rc";fi
mkdir "$art/qualification"
status=0
for transport in rdma tcp;do
 echo "HOUR_$transport" > "$art/stage"
 bash scripts/task_i_run.sh "$commit" "$art/qualification/$transport" "$transport" 172.16.4.150 measure-only
 rc=$?;echo "$rc" > "$art/$transport-exit"
 # Cleanup failures or missing workload receipt prohibit the next fixture.
 if [ "$rc" -eq 125 ] || [ "$rc" -eq 77 ] || [ ! -f "$art/qualification/$transport/measurement/exit" ];then exit "$rc";fi
 [ "$rc" -eq 0 ] || status=1
done
echo FINISHED > "$art/stage"
[ "$status" -eq 0 ] || exit "$status"
exit "$gate_rc"
