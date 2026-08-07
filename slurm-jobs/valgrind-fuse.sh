#!/bin/bash
#SBATCH --job-name=efs-vg-fuse
#SBATCH --partition=mit_normal
#SBATCH --time=00:45:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-fuse-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-fuse-%j.err

# Run efs-fuse under Valgrind against a real mount (servers not instrumented).

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
OUT="$SHARED/valgrind-fuse/${SLURM_JOB_ID}"
BASE="$SCRATCH/fuse-vg"
MNT="$BASE/mnt"
VGLOG="$OUT/efs-fuse.%p.txt"

mkdir -p "$SHARED/logs" "$OUT"
rm -rf "$SCRATCH"
mkdir -p "$BASE/s1" "$BASE/s2" "$BASE/s3" "$MNT"

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && kill "$FUSE_PID" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && wait "$FUSE_PID" 2>/dev/null || true
    for p in ${S1_PID:-} ${S2_PID:-} ${S3_PID:-}; do
      kill -TERM "$p" 2>/dev/null || true
    done
    sleep 1
    for p in ${S1_PID:-} ${S2_PID:-} ${S3_PID:-}; do
      kill -KILL "$p" 2>/dev/null || true
    done
    rm -rf "$SCRATCH"
}
trap cleanup EXIT TERM INT

cd "$REPO"

echo "=== host=$(hostname) job=$SLURM_JOB_ID $(date) ==="
valgrind --version
# Hide AVX-512 from glibc IFUNCs under Valgrind.
export GLIBC_TUNABLES="glibc.cpu.hwcaps=-AVX512F,-AVX512DQ,-AVX512VL,-AVX512BW,-AVX512CD"


echo "=== building -O1 -g (no AVX-512; Valgrind-safe libfuse) ==="
rm -f src/common/*.o src/client/*.o src/server/*.o src/mgmt/*.o src/query/*.o
rm -f deps/blake3/*.o libefs.a efsd efs-fuse efs-mgmt efs-query
VG_CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -DBLAKE3_NO_AVX512 -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-avx512dq -Wno-stringop-truncation -Wno-format-truncation -Ideps/libfuse/include -D_FILE_OFFSET_BITS=64'
FUSE_VG_CFLAGS='-O1 -g -fno-omit-frame-pointer -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-avx512dq -Wno-stringop-truncation -Wno-implicit-fallthrough -Wno-unused-result'
rm -f deps/libfuse/.efs-configured
if [ -f deps/libfuse/Makefile ]; then (cd deps/libfuse && make clean) || true; fi
(cd deps/libfuse && ./configure --disable-util --disable-example CFLAGS="$FUSE_VG_CFLAGS")
touch deps/libfuse/.efs-configured
make -j"${SLURM_CPUS_PER_TASK}" -C deps/libfuse
make -j"${SLURM_CPUS_PER_TASK}" CFLAGS="$VG_CFLAGS"

echo "=== starting 3 servers (not under valgrind) ==="
# Unique ports per job to avoid collisions with leftover efsd.
P1=$((18000 + SLURM_JOB_ID % 1000))
P2=$((P1 + 1))
P3=$((P1 + 2))
echo "ports $P1 $P2 $P3"

./efsd --node-id 1 --addr 127.0.0.1 --port "$P1" --storage "$BASE/s1" >"$BASE/s1.log" 2>&1 &
S1_PID=$!

wait_port() {
  local port=$1
  local waited=0
  while ! timeout 1 bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; do
    sleep 0.5
    waited=$((waited + 1))
    if [ "$waited" -ge 60 ]; then
      echo "Timed out waiting for port $port"
      echo "--- s1 ---"; cat "$BASE/s1.log" || true
      echo "--- s2 ---"; cat "$BASE/s2.log" || true
      echo "--- s3 ---"; cat "$BASE/s3.log" || true
      return 1
    fi
  done
}
wait_port "$P1"

# Join via --join (same path as Slurm server jobs); more reliable than add-node.
./efsd --node-id 2 --addr 127.0.0.1 --port "$P2" --storage "$BASE/s2" \
  --join "127.0.0.1:$P1" >"$BASE/s2.log" 2>&1 &
S2_PID=$!
wait_port "$P2"
sleep 1
./efsd --node-id 3 --addr 127.0.0.1 --port "$P3" --storage "$BASE/s3" \
  --join "127.0.0.1:$P1" >"$BASE/s3.log" 2>&1 &
S3_PID=$!
wait_port "$P3"
sleep 1

echo "--- server logs after join ---"
tail -5 "$BASE/s1.log" "$BASE/s2.log" "$BASE/s3.log" || true

./efs-mgmt status "127.0.0.1:$P1" || true
./efs-mgmt mkfs "127.0.0.1:$P1" vgfuse
./efs-mgmt list-exports "127.0.0.1:$P1"

VG_ARGS=(
  --tool=memcheck
  --leak-check=full
  --show-leak-kinds=definite,indirect
  --errors-for-leak-kinds=definite,indirect
  --track-origins=yes
  --error-exitcode=42
  --num-callers=30
  --trace-children=no
  --log-file="$VGLOG"
)
if [ -f "$REPO/slurm-jobs/valgrind-fuse.supp" ]; then
  VG_ARGS+=(--suppressions="$REPO/slurm-jobs/valgrind-fuse.supp")
fi

echo "=== mounting efs-fuse under valgrind ==="
valgrind "${VG_ARGS[@]}" \
  ./efs-fuse "127.0.0.1:$P1" vgfuse "$MNT" -f \
  >"$BASE/fuse.stdout" 2>"$BASE/fuse.stderr" &
FUSE_PID=$!

for i in $(seq 1 90); do
  if mountpoint -q "$MNT" 2>/dev/null; then
    break
  fi
  if ! kill -0 "$FUSE_PID" 2>/dev/null; then
    echo "efs-fuse exited before mount"
    cat "$BASE/fuse.stderr" || true
    ls -la "$OUT" || true
    exit 1
  fi
  sleep 1
done
if ! mountpoint -q "$MNT" 2>/dev/null; then
  echo "mount timed out"
  cat "$BASE/fuse.stderr" || true
  exit 1
fi
echo "mount OK (pid=$FUSE_PID)"

run() { timeout 60 "$@"; }

echo "=== FUSE smoke under valgrind ==="
run bash -c "echo 'hello vg fuse' > '$MNT/file.txt'"
run test "$(run cat "$MNT/file.txt")" = "hello vg fuse"
run chmod 700 "$MNT/file.txt"
run mkdir "$MNT/subdir"
run bash -c "echo nest > '$MNT/subdir/a.txt'"
run dd if=/dev/urandom of="$MNT/blob.bin" bs=64K count=4 status=none
SUM=$(run sha256sum "$MNT/blob.bin" | awk '{print $1}')
run sync "$MNT"
SUM2=$(run sha256sum "$MNT/blob.bin" | awk '{print $1}')
[ "$SUM" = "$SUM2" ]
run mv "$MNT/file.txt" "$MNT/renamed.txt"
run test -f "$MNT/renamed.txt"
run ln -s renamed.txt "$MNT/link.txt"
run test "$(run readlink "$MNT/link.txt")" = "renamed.txt"
run ln "$MNT/renamed.txt" "$MNT/hard.txt"
run truncate -s 5 "$MNT/renamed.txt"
run test "$(run wc -c < "$MNT/renamed.txt")" -eq 5
run rm -f "$MNT/hard.txt" "$MNT/link.txt" "$MNT/blob.bin"
run rm -f "$MNT/subdir/a.txt"
run rmdir "$MNT/subdir"
echo "smoke OK"

echo "=== unmount (triggers FUSE destroy/shutdown) ==="
fusermount -u "$MNT" || umount "$MNT"
for i in $(seq 1 60); do
  kill -0 "$FUSE_PID" 2>/dev/null || break
  sleep 1
done
if kill -0 "$FUSE_PID" 2>/dev/null; then
  echo "fuse still running; sending TERM"
  kill -TERM "$FUSE_PID" 2>/dev/null || true
  sleep 2
  kill -KILL "$FUSE_PID" 2>/dev/null || true
fi
wait "$FUSE_PID" 2>/dev/null || true
FUSE_PID=""

echo "=== valgrind summary ==="
fail=0
shopt -s nullglob
for f in "$OUT"/efs-fuse.*.txt; do
  echo "---- $(basename "$f") ----"
  grep -E 'ERROR SUMMARY|definitely lost|indirectly lost|Invalid |uninitialised|Syscall param|All heap blocks' "$f" || true
  if grep -qE 'ERROR SUMMARY: [1-9][0-9]* errors' "$f"; then
    fail=1
    # Print stacks for diagnosis
    grep -E 'definitely lost|Invalid |uninitialised|Syscall param| at 0x| by 0x' "$f" | head -80 || true
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "VALGRIND_FUSE_FAIL"
  exit 1
fi
echo "VALGRIND_FUSE_OK results in $OUT"
