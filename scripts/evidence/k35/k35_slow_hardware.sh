#!/usr/bin/env bash
# New private fixtures only; node9900 records its exact existing real client.
set -eu
src=${1:?source};art=${2:?fresh evidence};commit=${3:?source commit}
[[ $src == /tmp/efs-k35-slow-*/src && $art == /data1/efs/evidence/k35-slow-* ]] || exit 2
case $(hostname -s) in
 fstor007-mgmt) fuse=/home/erbmi1/orcd/scratch/efs/orcd-test/deps/libfuse;dev=mlx5_2;;
 node9900) fuse=/data1/efs/deps/libfuse;dev=mlx5_0;;
 *) exit 2;;
esac
mkdir "$art";exec >"$art/controller.log" 2>&1
trap 'rc=$?;echo "$rc" > "$art/controller-exit"' EXIT
cd "$src"
echo PREFLIGHT > "$art/stage"
for pass in 1 2;do
 if ! python3 - "$art" <<'PY'
import json,socket,sys
from pathlib import Path
from scripts.task_i_host_sample import foreign_jobs,background_identity
jobs=foreign_jobs()
if socket.gethostname().split('.')[0]=='node9900':
    token=background_identity(2380112)
    assert token['start_ticks']==353877425, 'pre-existing real client identity changed'
    Path(sys.argv[1],'background.json').write_text(json.dumps(token)+'\n')
    jobs=[j for j in jobs if j.get('pid')!=token['pid']]
if jobs:raise SystemExit('NOTRUN: hardware host occupied: '+repr(jobs))
PY
 then echo NOTRUN_OCCUPIED > "$art/stage";exit 77;fi
 [[ $pass != 1 ]] || sleep 20
done
export EFS_GIT_ID=$commit EFS_GIT_DESCRIBE=$commit EFS_GIT_BRANCH=agent/devel-k35-slow-fsync
export EFS_FUSE_PREFIX=$fuse EFS_RDMA_DEV=$dev
if [[ -d $fuse/lib64/pkgconfig ]];then lib=$fuse/lib64;else lib=$fuse/lib;fi
export PKG_CONFIG_PATH=$lib/pkgconfig LD_LIBRARY_PATH=$lib
export TMPDIR=$src/tmp EFS_B6_KEEP=1 EFS_K35_KEEP=1
unset LD_PRELOAD EFS_RDMA_GID_INDEX
mkdir -p "$TMPDIR"
{ date -u;hostname;uname -a;id;free -h;df -h /tmp;ip -4 -o addr;ibv_devinfo -d "$dev";pkg-config --modversion fuse3; } > "$art/environment.txt" 2>&1
echo BUILD > "$art/stage"
make -j8 all tests/faults/efsd FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,$lib" > "$art/build.log" 2>&1
{ ./efsd --version;sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd src/client/data_commit.c; } > "$art/identity.txt"
token=
[[ ! -f $art/background.json ]] || token=$art/background.json
python3 scripts/task_i_host_sample.py "$src" "$art/ownership.jsonl" "$token" > "$art/ownership.log" 2>&1 & monitor=$!
trap 'rc=$?;kill "$monitor" 2>/dev/null || true;wait "$monitor" 2>/dev/null || true;echo "$rc" > "$art/controller-exit"' EXIT
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
