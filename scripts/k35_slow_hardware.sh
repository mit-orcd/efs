#!/usr/bin/env bash
# New private fstor007 fixtures only. Never opens an existing cluster/store.
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?source commit}
[[ $(hostname -s) == fstor007-mgmt && $src == /tmp/efs-k35-slow-*/src && $art == /data1/efs/evidence/k35-slow-* ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$src"
echo PREFLIGHT > "$art/stage"
for pass in 1 2;do
 if ! python3 - <<'PY'
from scripts.task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
if jobs:raise SystemExit('NOTRUN: hardware host occupied: '+repr(jobs))
PY
 then echo NOTRUN_OCCUPIED > "$art/stage";exit 77;fi
 [[ $pass != 1 ]] || sleep 20
done
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-k35-slow-fsync
export EFS_FUSE_PREFIX=/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse
export PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib EFS_RDMA_DEV=mlx5_2
export TMPDIR=$src/tmp EFS_B6_KEEP=1 EFS_K35_KEEP=1
unset LD_PRELOAD EFS_RDMA_GID_INDEX
mkdir -p "$TMPDIR"
{ date -u;hostname;uname -a;id;free -h;df -h /tmp;ip -4 -o addr;ibv_devinfo -d mlx5_2;pkg-config --modversion fuse3; } > "$art/environment.txt" 2>&1
echo BUILD > "$art/stage"
make -j8 all tests/faults/efsd FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib" > "$art/build.log" 2>&1
{ ./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/client/data_commit.c; } > "$art/identity.txt"
echo RDMA_NATIVE > "$art/stage"
for name in xprt normal interrupt tcp-violation deadline burst burst-asym churn;do
 if [[ $name == xprt ]];then cmd=(./tests/test_rdma_xprt);else cmd=(./tests/test_rdma_segmented "$name");fi
 EFS_TRANSPORT=rdma timeout --signal=TERM --kill-after=10s 300 "${cmd[@]}" > "$art/native-$name.log" 2>&1 || rc=$?
 echo "${rc:-0}" > "$art/native-$name-exit"
 [[ ${rc:-0} == 0 ]] || exit "$rc"
 ! grep -qE 'NOTRUN|SKIP' "$art/native-$name.log" || exit 3
done
for transport in tcp rdma;do
 export EFS_TRANSPORT=$transport
 mkdir "$art/$transport"
 export EFS_B6_ARTIFACTS=$art/$transport
 echo "${transport}_B6_SLOW" > "$art/stage"
 EFS_B6_MODES=slow timeout --signal=TERM --kill-after=10s 900 python3 -u tests/test_b6_unavailable.py > "$art/$transport/b6-slow.log" 2>&1 || rc=$?
 echo "${rc:-0}" > "$art/$transport/b6-exit"
 [[ ${rc:-0} == 0 ]] || exit "$rc"
 echo "${transport}_K35" > "$art/stage"
 timeout --signal=TERM --kill-after=10s 900 python3 -u tests/test_k35_fsync_degraded.py > "$art/$transport/k35.log" 2>&1 || rc=$?
 echo "${rc:-0}" > "$art/$transport/k35-exit"
 [[ ${rc:-0} == 0 ]] || exit "$rc"
done
echo FINISHED > "$art/stage"
