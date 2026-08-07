#!/bin/bash
#SBATCH --job-name=efs-gdb-asan
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=24G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/gdb-asan-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/gdb-asan-%j.err
set -euo pipefail
REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
cd "$REPO"

# Use existing ASAN efsd if present; else build
if ! nm efsd 2>/dev/null | grep -q __asan_init; then
  make clean
  make -j4 efs-fuse efs-mgmt
  make -j4 efsd \
    CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -Wno-stringop-truncation -Wno-format-truncation -fsanitize=address' \
    LDFLAGS='-fsanitize=address -lpthread -lm -ldl'
fi

LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
export LOCAL
rm -rf "$LOCAL"
mkdir -p "$LOCAL"/{s1/d1,s1/d2,s1/d3,s1/d4,s2/d1,s2/d2,s2/d3,s2/d4,s3/d1,s3/d2,s3/d3,s3/d4,mnt,out,src}
cleanup() {
  fusermount -uz "$LOCAL/mnt" 2>/dev/null || true
  kill $FP $S1 $S2 $S3 2>/dev/null || true
  wait || true
  mkdir -p "$SHARED/logs/gdb-asan-${SLURM_JOB_ID}-out"
  cp -a "$LOCAL/out/." "$SHARED/logs/gdb-asan-${SLURM_JOB_ID}-out/" 2>/dev/null || true
  rm -rf "$LOCAL"
}
trap cleanup EXIT

python3 - <<'PY'
import os
root=os.environ["LOCAL"]+"/src"
os.makedirs(root+"/a", exist_ok=True)
for i in range(3000):
    open(f"{root}/a/f{i:04d}.bin","wb").write(os.urandom(65536 if i%3==0 else 4096))
print("src ready")
PY

export LOCAL
export ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:halt_on_error=1:print_stacktrace=1:fast_unwind_on_fatal=0
IP=127.0.0.1
P1=1961; P2=1962; P3=1963

# s1 under gdb
cat > "$LOCAL/out/gdbcmds" <<'GDB'
set pagination off
set print thread-events off
run
thread apply all bt 30
info registers
quit
GDB

gdb -batch -x "$LOCAL/out/gdbcmds" --args "$REPO/efsd" \
  --node-id 1 --addr "$IP" --port "$P1" --direct-io \
  --storage "$LOCAL/s1/d1,$LOCAL/s1/d2,$LOCAL/s1/d3,$LOCAL/s1/d4" --quota 50G \
  >"$LOCAL/out/s1.log" 2>&1 &
S1=$!
sleep 2
"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --direct-io \
  --storage "$LOCAL/s2/d1,$LOCAL/s2/d2,$LOCAL/s2/d3,$LOCAL/s2/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s2.log" 2>&1 & S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --direct-io \
  --storage "$LOCAL/s3/d1,$LOCAL/s3/d2,$LOCAL/s3/d3,$LOCAL/s3/d4" --quota 50G --join "$IP:$P1" \
  >"$LOCAL/out/s3.log" 2>&1 & S3=$!
for p in $P1 $P2 $P3; do
  for i in $(seq 1 60); do timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$p" 2>/dev/null && break; sleep 0.25; done
done
"$REPO/efs-mgmt" mkfs "$IP:$P1" asanfs
export FUSE_THREAD_STACK=8388608 EFS_META_BATCH_OPS=4096
"$REPO/efs-fuse" "$IP:$P1" asanfs "$LOCAL/mnt" -f >"$LOCAL/out/fuse.log" 2>&1 & FP=$!
for i in $(seq 1 40); do mountpoint -q "$LOCAL/mnt" && break; sleep 0.25; done

timeout 120 /home/erbmi1/git/direct_copy/ecopy "$LOCAL/src" "$LOCAL/mnt/dst" \
  >"$LOCAL/out/ecopy.log" 2>&1 || echo ecopy_rc=$?

sleep 3
echo '==== s1 (gdb) tail ===='
tail -120 "$LOCAL/out/s1.log" || true
echo '==== ASAN/gdb markers ===='
grep -E 'AddressSanitizer|SEGV|#[0-9]+ +0x|shard_io|write_fragment|heartbeat|server_handle|encode' "$LOCAL/out/s1.log" | tail -80 || true
