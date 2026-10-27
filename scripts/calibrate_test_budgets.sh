#!/bin/bash
# Run inside the pinned archived source tree, under a detached screen.
# Usage: bash /path/taskJ-calibrate.sh FULL_SOURCE_COMMIT ARTIFACT_ROOT
set -u
commit=$1
out=$2
case "$commit" in
  *[!0-9a-f]*|'') echo 'full hexadecimal source commit required' >&2; exit 2;;
esac
[ "${#commit}" -eq 40 ] || exit 2
[ -f Makefile ] && [ -f scripts/make_test_summary.sh ] || exit 2
mkdir -p "$out"
export EFS_GIT_ID=${commit:0:9} EFS_GIT_BRANCH=devel EFS_GIT_DESCRIBE=${commit:0:9}
export EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8
for n in 1 2 3; do
    dest=$out/run-$n
    mkdir "$dest" || exit 2
    printf '%s\n' "$commit" > "$dest/SOURCE"
    {
        date -u +%FT%TZ
        hostname
        uname -a
        uptime
        df -h "$PWD"
        ps -eo user,pid,ppid,etimes,comm,args
    } > "$dest/preflight.txt"
    printf 'run-%s RUNNING\n' "$n" > "$out/stage"
    bash scripts/make_test_summary.sh > "$dest/gate.log" 2>&1
    rc=$?
    printf '%s\n' "$rc" > "$dest/exit-status"
    python3 - "$dest" <<'PY'
from pathlib import Path
import shutil,sys
out=Path(sys.argv[1])
for name in ['summary.txt','timings.tsv','progress.txt','cmds.txt','cmds.raw','build.log','enum.log','build.failed','enum.failed']:
 p=Path(name)
 if p.is_file():shutil.copy2(p,out/name)
for p in Path('tmp').glob('fail-*.log'):shutil.copy2(p,out/p.name)
shutil.copy2('tests/budgets.tsv',out/'measured-budgets.tsv')
PY
    ./efsd --version > "$dest/build-identity.txt" 2>&1
    sha256sum efsd efs-fuse efs-mgmt > "$dest/binaries.sha256"
    date -u +%FT%TZ > "$dest/DONE"
    cat "$dest/summary.txt"
    # Host-limit skips may exit 3; failures never become calibration costs.
    if [ "$rc" -ne 0 ] && [ "$rc" -ne 3 ]; then
        printf 'run-%s FAILED; preserved; calibration stopped\n' "$n" > "$out/stage"
        exit "$rc"
    fi
    # Refuse overlapping runs if a leftover executable or cwd uses this tree.
    python3 - "$PWD" <<'PY'
from pathlib import Path
import os,sys
root=Path(sys.argv[1]).resolve()
ancestors={os.getpid()};pid=os.getppid()
while pid and pid not in ancestors:
 ancestors.add(pid)
 try:
  fields=(Path('/proc')/str(pid)/'status').read_text().splitlines()
  pid=int(next(line.split()[1] for line in fields if line.startswith('PPid:')))
 except (OSError,StopIteration):break
bad=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit() or int(p.name) in ancestors:continue
 for field in ['exe','cwd']:
  try:value=(p/field).resolve(strict=True)
  except OSError:continue
  if value==root or str(value).startswith(str(root)+'/'):bad.append((p.name,field,str(value)))
assert not bad, bad
PY
    [ "$?" -eq 0 ] || { printf 'LEFTOVER process; inspect before continuation\n' > "$out/stage"; exit 2; }
done
printf 'THREE_RUNS_COMPLETE\n' > "$out/stage"
