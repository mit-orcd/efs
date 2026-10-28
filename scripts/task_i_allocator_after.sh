#!/usr/bin/env bash
# Read-only allocator observation of a NEW private RDMA fixture after a pair.
# Diagnostic only: not a replacement qualification or a retry-to-PASS.
# COMMIT NEW_EVIDENCE_ROOT EXISTING_NODE9900_CLIENT_PID PRIOR_PAIR_ROOT
set -u
commit=${1:?source commit}; art=${2:?fresh evidence root}
client=${3:?pinned background client}; prior=${4:?prior pair root}
[[ $(hostname -s) == node9900 && $(pwd -P) == /tmp/efs-taskI-*/src ]] || exit 2
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$prior" == /data1/efs/evidence/taskI-* && "$art" == /data1/efs/evidence/taskI-* ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP
echo WAIT_PRIOR_PAIR > "$art/stage"
end=$((SECONDS+10800))
until [[ $(cat "$prior/stage" 2>/dev/null) == FINISHED && -f "$prior/controller-exit" ]];do
 (( SECONDS < end )) || exit 124
 sleep 20
done
# Reject an incomplete/unsafe predecessor. rc3 is a disclosed host limit.
python3 - "$prior" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1]);assert int((r/'controller-exit').read_text()) in (0,1,3)
for t in ('rdma','tcp'):
 p=r/'qualification'/t
 assert (p/'stage').read_text().strip()=='FINISHED'
 assert int((p/'controller-exit').read_text()) in (0,1)
 assert (p/'measurement/exit').exists()
PY
[[ $? == 0 ]] || exit 77
unset LD_PRELOAD TASK_I_ALLOC_DIR MALLOC_ARENA_MAX
export EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit" EFS_GIT_BRANCH=agent/devel-task-i
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_RDMA_DEV=mlx5_0
# task_i_run's all-account preflight refuses any other job and verifies the
# exact background identity. Only new efs-fuse processes load the observer.
echo BUILD_PROBE > "$art/stage"
gcc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread scripts/task_i_alloc_probe.c -o "$art/observer.so" > "$art/probe-build.log" 2>&1
rc=$?;echo "$rc" > "$art/probe-build-exit";[[ "$rc" == 0 ]] || exit "$rc"
mkdir "$art/allocator"
sha256sum scripts/task_i_alloc_probe.c "$art/observer.so" > "$art/probe.sha256"
echo DIAGNOSTIC_RDMA > "$art/stage"
export TASK_I_ALLOC_DIR="$art/allocator" LD_PRELOAD="$art/observer.so"
bash scripts/task_i_run.sh "$commit" "$art/diagnostic" rdma 172.16.99.0 measure-only "$client"
rc=$?;echo "$rc" > "$art/diagnostic-exit"
echo FINISHED > "$art/stage"
exit "$rc"
