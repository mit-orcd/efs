#!/usr/bin/env bash
# Fresh/private FIN candidate, after the preceding hardware suite has closed.
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?source commit};prior=${4:?closed hardware evidence}
[[ $(hostname -s) == node9900 && $src == /tmp/efs-k35-slow-fin*/src && $art == /data1/efs/evidence/fin-fullgate-* ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
[[ $(cat "$prior/controller-exit") == 0 && $(cat "$prior/strict-audit-exit") == 0 ]] || exit 77
cd "$src"
echo PREFLIGHT > "$art/stage"
for pass in 1 2;do
 python3 - "$art" <<'PY'
import json,sys,time
from pathlib import Path
from scripts.task_i_host_sample import foreign_jobs,background_identity
token=background_identity(2380112)
assert token['start_ticks']==353877425
jobs=[j for j in foreign_jobs() if j.get('pid')!=token['pid']]
with Path(sys.argv[1],'preflight.jsonl').open('a') as out:out.write(json.dumps(dict(t=time.time(),jobs=jobs,background=token))+'\n')
Path(sys.argv[1],'background.json').write_text(json.dumps(token)+'\n')
if jobs:raise SystemExit(77)
PY
 [[ $pass != 1 ]] || sleep 20
done
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-k35-slow-fsync
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_RDMA_DEV=mlx5_0 EFS_TRANSPORT=tcp
export EFS_B6_KEEP=1 EFS_K35_KEEP=1 EFS_B6_ARTIFACTS=$art/b6 TMPDIR=$src/tmp
unset LD_PRELOAD EFS_RDMA_GID_INDEX
python3 scripts/task_i_host_sample.py "$src" "$art/ownership.jsonl" "$art/background.json" > "$art/ownership.log" 2>&1 & monitor=$!
trap 'rc=$?;kill "$monitor" 2>/dev/null || true;wait "$monitor" 2>/dev/null || true;echo "$rc" > "$art/controller-exit"' EXIT
{ date -u;hostname;uname -a;id;free -h;df -h /tmp; } > "$art/environment.txt"
echo GATE > "$art/stage"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate.log" 2>&1 || rc=$?
echo "${rc:-0}" > "$art/gate-exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw;do [[ ! -f $f ]] || cp "$f" "$art/";done
for f in tmp/fail-*.log;do [[ ! -f $f ]] || cp "$f" "$art/";done
{ ./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/common/rdma.c src/common/tcp_counts.h; } > "$art/identity.txt"
echo FINISHED > "$art/stage"
exit "${rc:-0}"
