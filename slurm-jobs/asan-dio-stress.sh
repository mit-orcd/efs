#!/bin/bash
#SBATCH --job-name=efs-asan-dio
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=24G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/asan-dio-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/asan-dio-%j.err
set -euo pipefail
REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
cd "$REPO"

make clean
make -j4 efs-fuse efs-mgmt
make -j4 efsd \
  CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-stringop-truncation -Wno-format-truncation -fsanitize=address' \
  LDFLAGS='-fsanitize=address -lpthread -lm -ldl'

LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
export LOCAL
rm -rf "$LOCAL"
mkdir -p "$LOCAL"/{s1/d1,s1/d2,s1/d3,s1/d4,s2/d1,s2/d2,s2/d3,s2/d4,s3/d1,s3/d2,s3/d3,s3/d4,mnt,out,src}
cleanup() {
  fusermount -uz "$LOCAL/mnt" 2>/dev/null || true
  kill $FP $S1 $S2 $S3 2>/dev/null || true
  wait || true
  mkdir -p "$SHARED/logs/asan-dio-${SLURM_JOB_ID}-out"
  cp -a "$LOCAL/out/." "$SHARED/logs/asan-dio-${SLURM_JOB_ID}-out/" 2>/dev/null || true
  rm -rf "$LOCAL"
}
trap cleanup EXIT

# Generate ~2k small files + some larger ones (enough meta + data traffic)
python3 - <<'PY'
import os
root=os.environ["LOCAL"]+"/src"
os.makedirs(root+"/a", exist_ok=True)
os.makedirs(root+"/b", exist_ok=True)
for i in range(1500):
    open(f"{root}/a/f{i:04d}.bin","wb").write(os.urandom(4096 if i%7 else 65536))
for i in range(200):
    open(f"{root}/b/g{i:04d}.bin","wb").write(os.urandom(128*1024))
print("src ready", sum(len(fs) for _,_,fs in os.walk(root)))
PY

export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:halt_on_error=1:print_stacktrace=1
IP=127.0.0.1
P1=1961; P2=1962; P3=1963
"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --direct-io \
  --storage "$LOCAL/s1/d1,$LOCAL/s1/d2,$LOCAL/s1/d3,$LOCAL/s1/d4" --quota 50G \
  >"$LOCAL/out/s1.log" 2>&1 & S1=$!
sleep 1
"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --direct-io \
  --storage "$LOCAL/s2/d1,$LOCAL/s2/d2,$LOCAL/s2/d3,$LOCAL/s2/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s2.log" 2>&1 & S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --direct-io \
  --storage "$LOCAL/s3/d1,$LOCAL/s3/d2,$LOCAL/s3/d3,$LOCAL/s3/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s3.log" 2>&1 & S3=$!
for p in $P1 $P2 $P3; do
  for i in $(seq 1 40); do timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$p" 2>/dev/null && break; sleep 0.25; done
done
"$REPO/efs-mgmt" mkfs "$IP:$P1" asanfs
export FUSE_THREAD_STACK=8388608 EFS_META_BATCH_OPS=4096
"$REPO/efs-fuse" "$IP:$P1" asanfs "$LOCAL/mnt" -f >"$LOCAL/out/fuse.log" 2>&1 & FP=$!
for i in $(seq 1 40); do mountpoint -q "$LOCAL/mnt" && break; sleep 0.25; done
mountpoint -q "$LOCAL/mnt"

timeout 180 /home/erbmi1/git/direct_copy/ecopy "$LOCAL/src" "$LOCAL/mnt/dst" \
  >"$LOCAL/out/ecopy.log" 2>&1 || echo "ecopy_rc=$?"

sleep 2
alive=1
asan=0
for pid in $S1 $S2 $S3; do
  if ! kill -0 $pid 2>/dev/null; then alive=0; fi
done
for f in s1 s2 s3; do
  if grep -q 'AddressSanitizer\|ERROR:\|SUMMARY:\|Segmentation' "$LOCAL/out/$f.log"; then
    asan=1
    echo "==== ASAN in $f ===="
    grep -A40 'AddressSanitizer\|SUMMARY:\|ERROR:' "$LOCAL/out/$f.log" | head -50
  fi
done
echo ALIVE=$alive ASAN_HIT=$asan
tail -20 "$LOCAL/out/ecopy.log" || true
for f in s1 s2 s3 fuse; do echo "==== $f ===="; tail -15 "$LOCAL/out/$f.log"; done
[ "$asan" = 0 ] || exit 2
[ "$alive" = 1 ] || exit 3
echo STRESS_OK
