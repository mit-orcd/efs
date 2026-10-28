#!/usr/bin/env bash
# Real-host AF confirmation, new private fixtures only; never real stores.
set -eu
root=${1:?private root};art=${2:?fresh evidence};commit=${3:?source identity}
[[ $(hostname -s) == fstor007-mgmt && $root == /tmp/efs-task-af/real-* ]] || exit 2
[[ $art == /data1/efs/evidence/af-* ]] || exit 2
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$root/candidate/src"
export EFS_FUSE_PREFIX=/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse
export PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib/pkgconfig LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib
export EFS_RDMA_DEV=mlx5_2 EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-task-af
unset LD_PRELOAD EFS_RDMA_GID_INDEX
echo PREFLIGHT > "$art/stage"
for pass in 1 2;do
 if ! python3 - <<'PY'
from scripts.task_i_host_sample import foreign_jobs
jobs=foreign_jobs()
if jobs:raise SystemExit('NOTRUN: host occupied, no test launched: '+repr(jobs))
PY
 then echo NOTRUN_OCCUPIED > "$art/stage";exit 77;fi
 [[ $pass != 1 ]] || sleep 20
done
{ date -u;hostname;uname -a;id;free -h;df -h /tmp;ip -4 -o addr;ibv_devinfo -d mlx5_2;pkg-config --modversion fuse3; } > "$art/environment.txt" 2>&1
mkdir "$root/measure";cp -a "$root/candidate/src" "$root/measure/src"
python3 scripts/af_instrument.py "$root/measure/src"
echo BUILD > "$art/stage"
(cd "$root/measure/src";make -j8 all tests/faults/efsd FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$EFS_FUSE_PREFIX/lib") > "$art/build.log" 2>&1
(cd "$root/measure/src";./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/server/raft_host.c) > "$art/identity.txt"
python3 scripts/task_i_host_sample.py "$root" "$art/ownership.jsonl" > "$art/ownership.log" 2>&1 & monitor=$!
trap 'rc=$?;kill "$monitor" 2>/dev/null || true;wait "$monitor" 2>/dev/null || true;echo "$rc" > "$art/controller-exit"' EXIT
for transport in tcp rdma;do
 mkdir "$art/$transport";n=0
 for hz in 1 10 50 10 50 1 50 1 10;do
  n=$((n+1));echo "${transport}_${n}_${hz}Hz" > "$art/stage"
  (cd "$root/measure/src";python3 -u scripts/af_measure.py --out "$art/$transport/trial-$n-${hz}hz" --hz "$hz" --seconds 60 --rate 5 --transport "$transport" --address 172.16.4.150) > "$art/$transport/trial-$n-${hz}hz.log" 2>&1
 done
done
echo FINISHED > "$art/stage"
