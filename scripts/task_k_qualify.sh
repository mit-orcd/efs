#!/usr/bin/env bash
# Task K: assigned node9900, private /tmp cluster, no existing service changes.
# Usage: bash scripts/task_k_qualify.sh COMMIT ARTIFACTS RDMA_IPV4
set -u
set -o pipefail
commit=${1:?source commit}; art=${2:?new artifacts directory}; address=${3:?RDMA IPv4}
[[ "$commit" =~ ^[0-9a-f]{9,40}$ && "$PWD" == /tmp/efs-taskK-*/candidate ]] || exit 2
mkdir "$art" || exit 2
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
mkdir "$art/tcp" "$art/rdma" "$art/focused" "$art/gate" "$PWD/tmp"
export TMPDIR="$PWD/tmp" EFS_GIT_ID="$commit" EFS_GIT_BRANCH=agent/devel-task-k EFS_GIT_DESCRIBE="$commit"
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse
export PKG_CONFIG_PATH="$EFS_FUSE_PREFIX/lib64/pkgconfig" LD_LIBRARY_PATH="$EFS_FUSE_PREFIX/lib64"
export EFS_BUILD_JOBS=8
{ hostname; uname -a; nproc; free -h; df -h /tmp; gcc --version; pkg-config --modversion fuse3; ip -o -4 addr; ibv_devinfo -d mlx5_0; } > "$art/environment.txt" 2>&1
printf 'BUILD\n' > "$art/stage"
make -j8 FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib64" all tests/faults/efsd > "$art/build.log" 2>&1
rc=$?; echo "$rc" > "$art/build-exit"; [ "$rc" -eq 0 ] || exit "$rc"
./efsd --version > "$art/identity.txt" 2>&1
sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt > "$art/binaries.sha256"
failed=0
run() {
 local name=$1; shift
 printf '%s\n' "$name" > "$art/stage"
 "$@" > "$art/$name.log" 2>&1
 local rc=$?; echo "$rc" > "$art/$name-exit"
 [ "$rc" -eq 0 ] || failed=1
}
run tcp env EFS_TRANSPORT=tcp python3 -u tests/test_k44_retired_routing.py --port-min 19001 --out "$art/tcp"
run rdma env EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 python3 -u tests/test_k44_retired_routing.py --host "$address" --port-min 19101 --out "$art/rdma"
export EFS_TRANSPORT=tcp
for test in growth acceptance client_view autoid bookkeeping; do
 run "focused/k33-$test" python3 -u "tests/test_k33_$test.py"
done
run fast make test
printf 'GATE\n' > "$art/stage"
EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate/run.log" 2>&1
rc=$?; echo "$rc" > "$art/gate/exit"
for file in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw; do
 [ ! -f "$file" ] || cp "$file" "$art/gate/$file"
done
for file in tmp/fail-*.log; do [ ! -f "$file" ] || cp "$file" "$art/gate/"; done
printf 'FINISHED\n' > "$art/stage"
[ "$failed" -eq 0 ] || exit 1
exit "$rc"
