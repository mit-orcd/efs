#!/usr/bin/env bash
# Assigned host only. ROOT holds separate base/candidate/split exports.
# Refuses active AC campaigns, never signals or operates another cluster.
# ROOT EVIDENCE CANDIDATE_COMMIT
set -eu
root=${1:?private root};art=${2:?fresh evidence};candidate=${3:?candidate commit}
host=$(hostname -s)
case "$host" in
 intel1) [[ "$root" == "$HOME/efs-task-ac" ]]; transport=tcp;address=127.0.0.1;;
 node9901) [[ "$root" == /tmp/efs-task-ac ]];transport=rdma;address=10.1.99.1
  export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_RDMA_DEV=mlx5_0
  if [[ -d $EFS_FUSE_PREFIX/lib64/pkgconfig ]];then lib=$EFS_FUSE_PREFIX/lib64;else lib=$EFS_FUSE_PREFIX/lib;fi
  export PKG_CONFIG_PATH=$lib/pkgconfig LD_LIBRARY_PATH=$lib;;
 *) exit 2;;
esac
mkdir "$art"
exec >"$art/controller.log" 2>&1
trap 'rc=$?; echo "$rc" > "$art/controller-exit"' EXIT
mkdir "$root/lock" || exit 77
trap 'rc=$?; rmdir "$root/lock"; echo "$rc" > "$art/controller-exit"' EXIT
{ date -u; hostname; uname -a; id; free -h; df -h "$root"; ps -eo uid,pid,comm,args; } > "$art/environment.txt"
# Preserve background occupancy; no quiet-host claim is made.
mkdir "$root/split"
cp -a "$root/candidate/src" "$root/split/src"
for name in ac_measure.py ac_instrument.py ac_probe.h;do
 cp "$root/candidate/src/scripts/$name" "$root/base/src/scripts/$name"
done
for variant in base split;do
 echo "INSTRUMENT_BUILD_$variant" > "$art/stage"
 src=$root/$variant/src
 python3 "$root/candidate/src/scripts/ac_instrument.py" "$src"
 id=$candidate-ac
 [[ "$variant" != base ]] || id=1d2852da4-ac
 (cd "$src"; make -j8 EFS_GIT_ID="$id" EFS_GIT_BRANCH=agent/devel-task-ac \
   efsd efs-fuse efs-mgmt tests/faults/efsd) > "$art/build-$variant.log" 2>&1
 (cd "$src"; ./efsd --version; sha256sum efsd tests/faults/efsd efs-fuse efs-mgmt src/common/rdma.c src/server/iostats.c src/server/raft_host.c) > "$art/identity-$variant.txt"
done
# Six independent fresh fixtures, matched offered rate and balanced ordering.
round=0
for variant in base split split base base split;do
 round=$((round+1));echo "MEASURE_${round}_$variant" > "$art/stage"
 (cd "$root/$variant/src"; python3 -u scripts/ac_measure.py --out "$art/trial-$round-$variant" \
   --variant "$variant" --transport "$transport" --address "$address" --seconds 60 --rate 10 --writers 4) \
   > "$art/trial-$round-$variant.log" 2>&1
done
# Gate and native commands use the clean uninstrumented candidate export.
echo BUILD_CLEAN > "$art/stage"
cd "$root/candidate/src"
export EFS_GIT_ID=$candidate EFS_GIT_DESCRIBE=$candidate EFS_GIT_BRANCH=agent/devel-task-ac
make -j8 all tests/faults/efsd > "$art/build-clean.log" 2>&1
./efsd --version > "$art/clean-identity.txt"
sha256sum efsd efs-fuse efs-mgmt tests/faults/efsd > "$art/binaries.sha256"
./tests/test_iostats_snapshot > "$art/snapshot-unit.log" 2>&1
if [[ "$host" == intel1 ]];then
 echo GATE > "$art/stage"
 EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh > "$art/gate.log" 2>&1 || gate_rc=$?
 echo "${gate_rc:-0}" > "$art/gate-exit"
 for f in summary.txt timings.tsv progress.txt build.log enum.log cmds.txt cmds.raw;do [[ ! -f $f ]] || cp "$f" "$art/";done
 for f in tmp/fail-*.log;do [[ ! -f $f ]] || cp "$f" "$art/";done
else
 echo NATIVE_RDMA > "$art/stage"
 for name in xprt normal interrupt tcp-violation deadline burst burst-asym churn;do
  if [[ "$name" == xprt ]];then command=(./tests/test_rdma_xprt);else command=(./tests/test_rdma_segmented "$name");fi
  EFS_TRANSPORT=rdma timeout --signal=TERM --kill-after=10s 300 "${command[@]}" > "$art/native-$name.log" 2>&1
 done
fi
echo FINISHED > "$art/stage"
