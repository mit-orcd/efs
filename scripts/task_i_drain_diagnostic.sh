#!/usr/bin/env bash
# Numeric metadata-trigger attribution; deliberately NOT hour qualification.
set -u
art=${1:?fresh absolute evidence root}
[[ "$(hostname -s)" == amd-395 && "$(pwd -P)" == /home/maik/efs-taskI/.dirty-*/src ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
echo PREFLIGHT > "$art/stage"
for pass in 1 2; do
 python3 - <<'PY'
import sys
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
if jobs:raise SystemExit('NOTRUN: host occupied: '+repr(jobs))
PY
 [ "$?" -eq 0 ] || exit 77
 [ "$pass" != 1 ] || sleep 20
done
unset LD_PRELOAD MALLOC_ARENA_MAX TASK_I_ALLOC_DIR
mkdir -p "$PWD/tmp"
export TMPDIR="$PWD/tmp" EFS_TRANSPORT=tcp
{ date -u; hostname; uname -a; gcc --version; df -h "$TMPDIR"; } > "$art/environment.txt"
echo BUILD > "$art/stage"
make -j8 EFS_FAULTS=1 all tests/faults/efsd tests/test_kv_lsm tests/test_query_delta > "$art/build.log" 2>&1
rc=$?;echo "$rc" > "$art/build-exit";[ "$rc" -eq 0 ] || exit "$rc"
./efsd --version > "$art/build-identity.txt" 2>&1
sha256sum efsd tests/faults/efsd efs-fuse > "$art/binaries.sha256"
EFS_QUERY_STAGE_TRACE=1 ./tests/test_kv_lsm > "$art/kv-unit.log" 2>&1
rc=$?;echo "$rc" > "$art/kv-unit-exit";[ "$rc" -eq 0 ] || exit "$rc"
EFS_QUERY_STAGE_TRACE=1 ./tests/test_query_delta > "$art/query-unit.log" 2>&1
rc=$?;echo "$rc" > "$art/query-unit-exit";[ "$rc" -eq 0 ] || exit "$rc"
mkdir "$art/measurement"
echo DIAGNOSTIC > "$art/stage"
EFS_TRANSPORT=tcp EFS_QUERY_STAGE_TRACE=1 EFS_D4_LOAD_S=300 \
 EFS_D4_THREADS=8 EFS_D4_SET=256 EFS_D4_FG_SET=32 EFS_D4_QUOTA=8G \
 EFS_D4_DRAIN_S=150 EFS_D4_KEEP=1 EFS_D4_ARTIFACTS="$art/measurement" \
 timeout --signal=TERM 660 python3 -u scripts/task_i_load.py > "$art/measurement/run.log" 2>&1
rc=$?;echo "$rc" > "$art/measurement/exit"
python3 - "$art" <<'PY'
from pathlib import Path
import os,shutil,sys
src=Path.cwd();art=Path(sys.argv[1]);out=art/'logs';out.mkdir()
for work in (src/'tmp').glob('efs-d4p-*'):
 dest=out/work.name;dest.mkdir()
 for f in list(work.glob('*.log'))+list((work/'d').glob('*.log')):
  shutil.copyfile(f,dest/(('d-' if f.parent.name=='d' else '')+f.name))
left=[l for l in Path('/proc/self/mountinfo').read_text().splitlines() if str(src/'tmp')+'/' in l]
for p in Path('/proc').glob('[0-9]*'):
 try:
  if p.stat().st_uid==os.getuid() and (p/'exe').resolve() in (src/'efs-fuse',src/'tests/faults/efsd'):left.append(str(p))
 except OSError:pass
if left:raise SystemExit('CLEANUP_BLOCKED: '+repr(left))
PY
[ "$?" -eq 0 ] || { echo CLEANUP_BLOCKED > "$art/stage";exit 125; }
echo FINISHED > "$art/stage"
exit "$rc"
