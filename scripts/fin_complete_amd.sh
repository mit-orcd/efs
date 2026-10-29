#!/usr/bin/env bash
# Full remaining-prerequisite qualification on an actually free AMD host.
# SRC FRESH_EVIDENCE SOURCE_COMMIT CODE_ONLY_CLUSTER_SCRIPTS
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?commit};cluster=${4:?code-only cluster scripts}
[[ $(hostname -s) == amd-395 && $src == "$HOME/efs-fin-complete-"*/src && $art == "$HOME/efs-artifacts/fin-complete-"* && $cluster == "${src%/src}/cluster-scripts" ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$src"
echo PREFLIGHT > "$art/stage"
for pass in 1 2;do
 python3 - "$art" <<'PY'
from pathlib import Path
import json,sys,time
from scripts.task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
with Path(sys.argv[1],'preflight.jsonl').open('a') as out:out.write(json.dumps(dict(t=time.time(),foreign=jobs))+'\n')
assert not jobs,jobs
assert 'efs-fuse' not in Path('/proc/self/mountinfo').read_text()
PY
 [[ $pass != 1 ]] || sleep 20
done
[[ ! -e $cluster/env.sh && ! -e $cluster/hosts.sh && ! -e $cluster/.current-cluster ]] || exit 2
for f in cluster.sh ct-up.sh gw-up.sh lib/client-stop.sh lib/client-user.sh lib/transfer.sh;do test -f "$cluster/$f";done
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-k35-slow-fsync
export EFS_CLUSTER_SCRIPTS=$cluster EFS_TRANSPORT=auto EFS_RDMA_DEV=rxe0 EFS_FUSE_PREFIX=/usr
export TMPDIR=$src/tmp EFS_B6_KEEP=1 EFS_K35_KEEP=1
unset LD_PRELOAD ASAN_OPTIONS LSAN_OPTIONS TSAN_OPTIONS EFS_SAN_ONLY EFS_LEAK_ONLY EFS_TEST_CMD_TIMEOUT EFS_TEST_TSAN EFS_TEST_SANITIZE EFS_SANITIZE
mkdir -p "$TMPDIR" "$art/focused" "$art/gate"
{ date -u;hostname;uname -a;id;free -h;df -h "$src";gcc --version;valgrind --version;pkg-config --modversion fuse3;ibv_devinfo -d rxe0;cat /proc/sys/kernel/yama/ptrace_scope;ulimit -a; } > "$art/environment.txt" 2>&1
command -v valgrind > "$art/valgrind-path.txt"
test -r /usr/include/valgrind/memcheck.h
{ cat "${src%/src}/cluster-source.txt";cd "$cluster";sha256sum cluster.sh ct-up.sh gw-up.sh lib/client-stop.sh lib/client-user.sh lib/transfer.sh; } > "$art/cluster-source.txt"
python3 scripts/task_i_host_sample.py "$src" "$art/ownership.jsonl" > "$art/ownership.log" 2>&1 & monitor=$!
trap 'rc=$?;kill "$monitor" 2>/dev/null || true;wait "$monitor" 2>/dev/null || true;echo "$rc" > "$art/controller-exit"' EXIT
run() {
 local name=$1 seconds=$2;shift 2
 local code=0
 timeout --signal=TERM --kill-after=10s "$seconds" "$@" > "$art/focused/$name.log" 2>&1 || code=$?
 echo "$code" > "$art/focused/$name-exit"
 [[ $code == 0 ]] || return "$code"
 if grep -qE 'NOTRUN|SKIP' "$art/focused/$name.log";then return 3;fi
}
echo RUNTIME > "$art/stage"
run asan-lsan-runtime 30 python3 -c 'import sys;sys.path.insert(0,"tests");from sanitize_env import asan_runtime_available;assert asan_runtime_available();print("Independent ASan/LSan runtime PASS")'
echo BUILD > "$art/stage"
make -j8 all tests/faults/efsd > "$art/build.log" 2>&1
{ ./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/common/rdma.c src/common/tcp_counts.h; } > "$art/identity.txt"
echo FOCUSED > "$art/stage"
run cluster-stop 900 python3 tests/test_cluster_stop.py
run rdma-memcheck 900 python3 tests/test_rdma_memcheck.py
run leak-gate 900 python3 tests/test_leak_gate.py
run sanitizer-gate 900 python3 tests/test_sanitizer_gate.py
grep -Fq 'sanitizer gate (tsan): racy fixture -> FAIL with the report, clean sibling PASS' "$art/focused/sanitizer-gate.log"
grep -Fq 'sanitizer gate (asan): heap overflow and signed overflow -> FAIL with the report, clean sibling PASS' "$art/focused/sanitizer-gate.log"
grep -Fq 'sanitizer gate (asan): leaking fixture -> FAIL (LeakSanitizer) PASS' "$art/focused/sanitizer-gate.log"
run declaration 300 ./tests/test_declaration_gate
run xprt 300 ./tests/test_rdma_xprt
for mode in normal interrupt tcp-violation deadline burst burst-asym churn;do run "$mode" 300 ./tests/test_rdma_segmented "$mode";done
echo GATE > "$art/stage"
export EFS_B6_ARTIFACTS=$art/gate/b6
code=0
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1 || code=$?
echo "$code" > "$art/gate/exit"
for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw;do [[ ! -f $f ]] || cp "$f" "$art/gate/";done
for f in tmp/fail-*.log;do [[ ! -f $f ]] || cp "$f" "$art/gate/";done
echo FINISHED > "$art/stage"
exit "$code"
