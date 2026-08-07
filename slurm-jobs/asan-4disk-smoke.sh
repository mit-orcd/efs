#!/bin/bash
#SBATCH --job-name=efs-asan-smoke
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/asan-smoke-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/asan-smoke-%j.err
set -euo pipefail
REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
cd "$REPO"

# Normal fuse/mgmt + ASAN efsd only (libfuse configure fails under full ASAN).
make clean
make -j4 efs-fuse efs-mgmt
make -j4 efsd \
  CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-stringop-truncation -Wno-format-truncation -fsanitize=address' \
  LDFLAGS='-fsanitize=address -lpthread -lm -ldl'

LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
rm -rf "$LOCAL"
mkdir -p "$LOCAL"/{s1/d1,s1/d2,s1/d3,s1/d4,s2/d1,s2/d2,s2/d3,s2/d4,s3/d1,s3/d2,s3/d3,s3/d4,mnt,out}
cleanup() {
  fusermount -uz "$LOCAL/mnt" 2>/dev/null || true
  kill $FP $S1 $S2 $S3 2>/dev/null || true
  wait || true
  rm -rf "$LOCAL"
}
trap cleanup EXIT

export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:halt_on_error=1
IP=127.0.0.1
P1=1961; P2=1962; P3=1963
"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" \
  --storage "$LOCAL/s1/d1,$LOCAL/s1/d2,$LOCAL/s1/d3,$LOCAL/s1/d4" --quota 50G \
  >"$LOCAL/out/s1.log" 2>&1 & S1=$!
sleep 1
"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" \
  --storage "$LOCAL/s2/d1,$LOCAL/s2/d2,$LOCAL/s2/d3,$LOCAL/s2/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s2.log" 2>&1 & S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" \
  --storage "$LOCAL/s3/d1,$LOCAL/s3/d2,$LOCAL/s3/d3,$LOCAL/s3/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s3.log" 2>&1 & S3=$!
for p in $P1 $P2 $P3; do
  for i in $(seq 1 40); do timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$p" 2>/dev/null && break; sleep 0.25; done
done
"$REPO/efs-mgmt" mkfs "$IP:$P1" asanfs
export FUSE_THREAD_STACK=8388608 EFS_META_BATCH_OPS=4096
"$REPO/efs-fuse" "$IP:$P1" asanfs "$LOCAL/mnt" -f >"$LOCAL/out/fuse.log" 2>&1 & FP=$!
for i in $(seq 1 40); do mountpoint -q "$LOCAL/mnt" && break; sleep 0.25; done

SRC=/home/erbmi1/orcd/scratch/imagenet/images_complete/ilsvrc/val
# Cap runtime: enough load to hit usage/meta paths; not a full copy.
timeout 120 /home/erbmi1/git/direct_copy/ecopy "$SRC" "$LOCAL/mnt/val" \
  >"$LOCAL/out/ecopy.log" 2>&1 || true
sleep 2

alive=1
for pid in $S1 $S2 $S3 $FP; do
  if ! kill -0 $pid 2>/dev/null; then alive=0; fi
done
echo ALIVE=$alive
mkdir -p "$SHARED/logs/asan-smoke-${SLURM_JOB_ID}-out"
cp -a "$LOCAL/out/." "$SHARED/logs/asan-smoke-${SLURM_JOB_ID}-out/"
for f in s1 s2 s3 fuse; do
  echo "==== $f ===="
  tail -40 "$LOCAL/out/$f.log" || true
done
grep -E 'AddressSanitizer|ERROR:|SUMMARY:|Segmentation' "$LOCAL/out"/*.log && exit 2 || true
[ "$alive" = 1 ] || { echo SMOKE_FAIL_DEAD; exit 3; }
echo SMOKE_OK
