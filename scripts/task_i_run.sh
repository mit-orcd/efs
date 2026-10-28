#!/usr/bin/env bash
# Task I, F1: one unchanged 60-minute fixed-set load, then the full code gate.
# Run only on the assigned quiet host, from a private exported source tree.
# Usage: bash scripts/task_i_run.sh COMMIT ARTIFACTS [tcp|rdma] [HOST_IPV4] [full|measure-only] [EXISTING_NODE9900_CLIENT_PID]
set -u
commit=${1:?exact exported commit required}
art=${2:?new external artifacts directory required}
transport=${3:-tcp}
address=${4:-127.0.0.1}
gate_mode=${5:-full}
background_client=${6:-}
[[ "$gate_mode" == full || "$gate_mode" == measure-only ]] || exit 2
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$transport" =~ ^(tcp|rdma)$ ]] || exit 2
src=$(pwd -P)
[[ "$src" == "$HOME/efs-taskI/src" || "$src" == "$HOME"/efs-taskI/*/src || "$src" == /tmp/efs-taskI-*/src ]] || exit 2
[[ ! -e "$HOME/efs-taskH" ]] || { echo 'NOTRUN: task H still owns its private tree'; exit 77; }
mkdir "$art" || exit 2
mkdir "$art/measurement" "$art/gate" "$art/logs" "$src/tmp"
exec >"$art/controller.log" 2>&1
trap 'rc=$?; printf "%s\n" "$rc" > "$art/controller-exit"' EXIT
export TMPDIR="$src/tmp" EFS_GIT_ID="$commit" EFS_GIT_BRANCH=agent/devel-task-i EFS_GIT_DESCRIBE="$commit"
export EFS_FUSE_PREFIX=${EFS_FUSE_PREFIX:-/usr}
export EFS_TRANSPORT="$transport"
export PATH="$HOME/bin:$PATH"
# Refuse other test jobs; never signal them. No other cluster is stopped.
python3 - "$background_client" "$art" <<'PY'
import json,sys
from pathlib import Path
sys.path.insert(0,'scripts')
from task_i_host_sample import foreign_jobs
from task_i_host_sample import background_identity
allowed = background_identity(sys.argv[1]) if sys.argv[1] else None
jobs=foreign_jobs()
if allowed:
    (Path(sys.argv[2])/'background-client.json').write_text(json.dumps(allowed,indent=2)+'\n')
    jobs=[job for job in jobs if job['pid']!=allowed['pid']]
if jobs:raise SystemExit('NOTRUN: quiet host required: '+repr(jobs))
PY
[ "$?" -eq 0 ] || exit 77
printf 'BUILD\n' > "$art/stage"
{ hostname; id; uname -a; nproc; free -h; df -h "$HOME" "$TMPDIR"; gcc --version; pkg-config --modversion fuse3; cat /sys/kernel/mm/transparent_hugepage/enabled; } > "$art/environment.txt" 2>&1
if [ -d "$EFS_FUSE_PREFIX/lib64/pkgconfig" ]; then
 export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib64/pkgconfig" LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib64"
fi
fuse_libs="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib64"
make -j8 FUSE_LIBS="$fuse_libs" all tests/faults/efsd > "$art/build.log" 2>&1
rc=$?; printf '%s\n' "$rc" > "$art/build-exit"
[ "$rc" -eq 0 ] || exit "$rc"
./efsd --version > "$art/build-identity.txt" 2>&1
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt > "$art/binaries.sha256"
printf 'MEASUREMENT\n' > "$art/stage"
python3 -u scripts/rss_mappings_sample.py "${src%/src}/" "$art/measurement/mem.jsonl" 10 > "$art/measurement/sampler.log" 2>&1 &
sampler=$!
python3 -u scripts/task_i_host_sample.py "$src" "$art/measurement/host.jsonl" "${background_client:+$art/background-client.json}" > "$art/measurement/host-sampler.log" 2>&1 &
vmstat_pid=$!
load_args=(--host-address "$address")
if [ -n "${TASK_I_ALLOC_DIR:-}" ]; then
    load_args+=(--allocator-start-marker "$TASK_I_ALLOC_DIR/load-start")
fi
EFS_D4_LOAD_S=3600 EFS_D4_SET=256 EFS_D4_THREADS=8 EFS_D4_FG_SET=32 \
EFS_D4_DRAIN_S=150 EFS_D4_QUOTA=8G EFS_D4_KEEP=1 EFS_D4_ARTIFACTS="$art/measurement" \
python3 -u scripts/task_i_load.py "${load_args[@]}" > "$art/measurement/run.log" 2>&1
measure_rc=$?
kill "$sampler" "$vmstat_pid"
wait "$sampler" "$vmstat_pid" 2>/dev/null || :
printf '%s\n' "$measure_rc" > "$art/measurement/exit"
python3 - "$art" "$src" <<'PY'
from pathlib import Path
import json,os,shutil,sys
art,src=map(Path,sys.argv[1:]);sys.path.insert(0,str(src/'scripts'))
import rss_plateau
host_rows=[json.loads(x) for x in (art/'measurement/host.jsonl').read_text().splitlines()]
foreign=[r for r in host_rows if r['foreign_jobs']]
shared=any(r.get('background_jobs') for r in host_rows)
(art/'measurement/quiet-host.json').write_text(json.dumps(dict(verdict='FAIL' if foreign else ('SHARED_CLIENT' if shared else 'PASS'),overlaps=foreign,background_client_present=shared),indent=2)+'\n')
# Preserve closed daemon and mount logs BEFORE any scratch pruning.
for work in (src/'tmp').glob('efs-d4p-*'):
 dest=art/'logs'/work.name;dest.mkdir()
 for p in list(work.glob('*.log'))+list((work/'d').glob('*.log')):
  shutil.copyfile(p,dest/(('d-' if p.parent.name=='d' else '')+p.name))
left=[]
for p in Path('/proc').glob('[0-9]*'):
 try:
  if p.stat().st_uid==os.getuid() and (p/'exe').resolve() in (src/'tests/faults/efsd',src/'efs-fuse'):left.append(str(p))
 except OSError:pass
left += [x for x in Path('/proc/self/mountinfo').read_text().splitlines() if str(src/'tmp')+'/' in x]
if left:
 print('OWNED CLEANUP BLOCKED: '+repr(left));raise SystemExit(125)
valid=False
f=art/'measurement/d4-result.json'
if f.exists():
 doc=json.loads(f.read_text());start=doc.get('measured',{}).get('load_started_epoch_s')
 if start is not None:
  series={}
  for line in (art/'measurement/mem.jsonl').read_text().splitlines():
   r=json.loads(line);t=r['t']-start
   if not 0<=t<=3600:continue
   for name,p in r['p'].items():series.setdefault(name+':'+str(p['pid']),[]).append((t,p['rss']))
  rows=rss_plateau.evaluate(series,3600,5)
  valid=len(rows)==5 and all(r['samples']>=350 and r['span_s']>=3490 for r in rows)
  result=dict(rows=rows,complete=valid,scope='shared-client-host' if shared else 'quiet-host',verdict='PASS' if valid and not foreign and all(r['verdict']=='PLATEAU' for r in rows) else 'FAIL')
  (art/'measurement/rss-10s.json').write_text(json.dumps(result,indent=2)+'\n')
  print(rss_plateau.format_rows(rows))
  valid=result['verdict']=='PASS'
raise SystemExit(0 if valid else 1)
PY
analysis_rc=$?
printf '%s\n' "$analysis_rc" > "$art/measurement/analysis-exit"
[ "$analysis_rc" -ne 125 ] || { printf 'CLEANUP_BLOCKED\n' > "$art/stage"; exit 125; }
[ "$analysis_rc" -eq 0 ] || measure_rc=1
if [ "$gate_mode" == measure-only ]; then
 printf 'FINISHED\n' > "$art/stage"
 exit "$measure_rc"
fi
printf 'GATE\n' > "$art/stage"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
gate_rc=$?
printf '%s\n' "$gate_rc" > "$art/gate/exit"
for name in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$name" ] || cp "$name" "$art/gate/$name"
done
for name in tmp/fail-*.log; do [ ! -f "$name" ] || cp "$name" "$art/gate/"; done
printf 'FINISHED\n' > "$art/stage"
[ "$measure_rc" -eq 0 ] || exit "$measure_rc"
exit "$gate_rc"
