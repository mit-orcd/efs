#!/bin/bash
# Task O private xorinox acceptance, with retained outcomes and fixture logs.
# Usage: qualify_dir_ops.sh PRIVATE_ROOT OUTPUT SOURCE_COMMIT [BASE_DONE_FILE]
set -eu
private=$1
out=$2
commit=$3
wait_for=${4:-}
cd "$(dirname "$0")/.."
source_root=$PWD
mkdir -p "$private/tmp" "$out"
export TMPDIR="$private/tmp" EFS_GIT_ID="$commit" EFS_GIT_DESCRIBE="$commit"
export EFS_GIT_BRANCH=agent/devel-task-o EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8
# Test retention only: no product behavior or success criterion changes.
export EFS_K36_KEEP=1 EFS_D4_KEEP=1
printf 'build\n' > "$out/phase"
make -j8 test-bins > "$out/build.log" 2>&1
./efsd --version > "$out/build-id.txt" 2>&1
./tests/faults/efsd --version >> "$out/build-id.txt" 2>&1
printf 'waiting-for-load-pools\n' > "$out/phase"
while [ -n "$wait_for" ] && [ ! -f "$wait_for" ]; do sleep 10; done
run() {
    name=$1; shift
    printf '%s\n' "$name" > "$out/phase"
    start=$(date +%s)
    set +e
    "$@" > "$out/$name.log" 2>&1
    rc=$?
    set -e
    printf '%s\n' "$rc" > "$out/$name.exit"
    printf '%s\t%s\t%s\n' "$name" "$rc" "$(( $(date +%s) - start ))" >> "$out/runs.tsv"
    # INCOMPLETE (3) belongs only to the summary runner, not ordinary tests.
    if [ "$rc" != 0 ]; then return "$rc"; fi
}
finish() {
    rc=$?
    trap - EXIT
    printf '%s\n' "$rc" > "$out/controller.exit"
    # Preserve original logs after test finally blocks have stopped daemons.
    python3 - "$private" "$source_root" "$out" <<'PY'
import hashlib, json, shutil, sys
from pathlib import Path
private, src, out = map(Path, sys.argv[1:])
for prefix, base in [('private', private/'tmp'), ('runner', src/'tmp')]:
    if not base.exists():
        continue
    for p in base.rglob('*.log'):
        if p.is_file():
            target=out/'fixture-logs'/prefix/p.relative_to(base)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(p,target)
for name in ['summary.txt','timings.tsv','progress.txt','cmds.txt','enum.log']:
    p=src/name
    if p.is_file(): shutil.copy2(p,out/('gate-'+name))
files=sorted(p for p in out.rglob('*') if p.is_file() and p.name!='SHA256SUMS')
(out/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(out))+'\n' for p in files))
PY
    exit "$rc"
}
trap finish EXIT
run dir-busy python3 tests/test_dir_ops_busy.py
run fast make test
for test in test_k36_recovery test_w159_apply_wait; do
    for n in 1 2 3 4 5; do run "$test-$n" python3 "tests/$test.py"; done
done
run core python3 scripts/qualify_dir_ops.py --work "$private/fixed-core" --output "$out/core" --rounds 1
printf 'full-gate\n' > "$out/phase"
set +e
EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh > "$out/gate-controller.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$out/gate.exit"
case "$rc" in 0|3) ;; *) exit "$rc";; esac
printf 'complete\n' > "$out/phase"
