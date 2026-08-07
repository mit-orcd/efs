#!/bin/bash
#SBATCH --job-name=efs-vg-hot
#SBATCH --partition=mit_quicktest,mit_normal
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-hotpath-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-hotpath-%j.err
#
# Valgrind memcheck on efs-fuse while running the hot-path smoke (servers bare).

set -euo pipefail

REPO="/home/erbmi1/git/efs"
DUT_SRC="/home/erbmi1/git/dut"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
OUT="$SHARED/valgrind-hotpath/${SLURM_JOB_ID}"
BASE="$SCRATCH/vg-hot"
MNT="$BASE/mnt"
MNT2="$BASE/mnt2"
VGLOG="$OUT/efs-fuse.%p.txt"

mkdir -p "$SHARED/logs" "$OUT"
rm -rf "$SCRATCH"
mkdir -p "$BASE/s1" "$BASE/s2" "$BASE/s3" "$MNT" "$MNT2"

FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    fusermount -u "$MNT2" 2>/dev/null || umount "$MNT2" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && kill "$FUSE_PID" 2>/dev/null || true
    [ -n "${FUSE2_PID:-}" ] && kill "$FUSE2_PID" 2>/dev/null || true
    wait "${FUSE_PID:-}" 2>/dev/null || true
    wait "${FUSE2_PID:-}" 2>/dev/null || true
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

echo "=== build dut + efs -O1 -g ==="
make -C "$DUT_SRC" -j"${SLURM_CPUS_PER_TASK}"
DUT="$DUT_SRC/dut"
rm -f src/common/*.o src/client/*.o src/server/*.o src/mgmt/*.o src/query/*.o
rm -f deps/blake3/*.o libefs.a efsd efs-fuse efs-mgmt efs-query
make -j"${SLURM_CPUS_PER_TASK}" deps/libfuse/lib/.libs/libfuse.a
# Valgrind 3.22 cannot decode many AVX-512 ops. Rebuild libfuse + efs without them.
VG_CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -DBLAKE3_NO_AVX512 -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-avx512dq -Wno-stringop-truncation -Wno-format-truncation -Ideps/libfuse/include -D_FILE_OFFSET_BITS=64'
FUSE_VG_CFLAGS='-O1 -g -fno-omit-frame-pointer -mno-avx512f -mno-avx512vl -mno-avx512bw -mno-avx512dq -Wno-stringop-truncation -Wno-implicit-fallthrough -Wno-unused-result'
rm -f deps/libfuse/.efs-configured
if [ -f deps/libfuse/Makefile ]; then
  (cd deps/libfuse && make clean) || true
fi
(cd deps/libfuse && ./configure --disable-util --disable-example CFLAGS="$FUSE_VG_CFLAGS")
touch deps/libfuse/.efs-configured
make -j"${SLURM_CPUS_PER_TASK}" -C deps/libfuse
make -j"${SLURM_CPUS_PER_TASK}" CFLAGS="$VG_CFLAGS"

P1=$((18000 + SLURM_JOB_ID % 1000))
P2=$((P1 + 1))
P3=$((P1 + 2))
echo "ports $P1 $P2 $P3"

wait_listen() {
  local log=$1 pid=$2 label=$3
  local i
  for i in $(seq 1 80); do
    if grep -q "listening on" "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "$label died before listen"; cat "$log"; cp -f "$log" "$OUT/" 2>/dev/null || true
      return 1
    fi
    sleep 0.25
  done
  echo "$label listen timeout"; cat "$log"; cp -f "$log" "$OUT/" 2>/dev/null || true
  return 1
}

./efsd --node-id 1 --addr 127.0.0.1 --port "$P1" --storage "$BASE/s1" --writers 4 \
    >"$BASE/s1.log" 2>&1 &
S1_PID=$!
wait_listen "$BASE/s1.log" "$S1_PID" s1 || exit 1
./efsd --node-id 2 --addr 127.0.0.1 --port "$P2" --storage "$BASE/s2" \
  --join "127.0.0.1:$P1" --writers 4 >"$BASE/s2.log" 2>&1 &
S2_PID=$!
wait_listen "$BASE/s2.log" "$S2_PID" s2 || exit 1
./efsd --node-id 3 --addr 127.0.0.1 --port "$P3" --storage "$BASE/s3" \
  --join "127.0.0.1:$P1" --writers 4 >"$BASE/s3.log" 2>&1 &
S3_PID=$!
wait_listen "$BASE/s3.log" "$S3_PID" s3 || exit 1
sleep 1

./efs-mgmt mkfs "127.0.0.1:$P1" vghot || { echo mkfs failed; exit 1; }

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

echo "=== mount primary under valgrind ==="
# Hide AVX-512 from glibc IFUNCs; Valgrind 3.22 still trips on some EVEX paths.
export GLIBC_TUNABLES="${GLIBC_TUNABLES:+$GLIBC_TUNABLES:}glibc.cpu.hwcaps=-AVX512F,-AVX512DQ,-AVX512VL,-AVX512BW,-AVX512CD"
valgrind "${VG_ARGS[@]}" \
  ./efs-fuse "127.0.0.1:$P1" "127.0.0.1:$P2" "127.0.0.1:$P3" vghot "$MNT" -f \
  >"$BASE/fuse.stdout" 2>"$BASE/fuse.stderr" &
FUSE_PID=$!

for i in $(seq 1 120); do
  mountpoint -q "$MNT" 2>/dev/null && break
  kill -0 "$FUSE_PID" 2>/dev/null || { cat "$BASE/fuse.stderr"; exit 1; }
  sleep 1
done
mountpoint -q "$MNT" || { echo "mount timed out"; exit 1; }

echo "=== second mount (not under valgrind) ==="
./efs-fuse "127.0.0.1:$P1" "127.0.0.1:$P2" "127.0.0.1:$P3" vghot "$MNT2" -f \
  >"$BASE/fuse2.stdout" 2>&1 &
FUSE2_PID=$!
for i in $(seq 1 60); do
  mountpoint -q "$MNT2" 2>/dev/null && break
  sleep 0.5
done
mountpoint -q "$MNT2" || { fail "mount2"; cat "$BASE/fuse2.stdout"; exit 1; }

HOTPATH_TMP="$BASE"
# shellcheck source=lib-hotpath-smoke.sh
source "$REPO/slurm-jobs/lib-hotpath-smoke.sh"
efs_hotpath_run_all "$MNT" "$MNT2"

echo "=== unmount ==="
fusermount -u "$MNT2" 2>/dev/null || true
fusermount -u "$MNT" 2>/dev/null || true
for i in $(seq 1 90); do
  kill -0 "$FUSE_PID" 2>/dev/null || break
  sleep 1
done
kill -TERM "$FUSE_PID" 2>/dev/null || true
sleep 2
kill -KILL "$FUSE_PID" 2>/dev/null || true
wait "$FUSE_PID" 2>/dev/null || true
FUSE_PID=""
kill -TERM "$FUSE2_PID" 2>/dev/null || true
wait "$FUSE2_PID" 2>/dev/null || true
FUSE2_PID=""

echo "=== valgrind summary ==="
vgfail=0
shopt -s nullglob
for f in "$OUT"/efs-fuse.*.txt; do
  echo "---- $(basename "$f") ----"
  grep -E 'ERROR SUMMARY|definitely lost|indirectly lost|Invalid |uninitialised|Syscall param|All heap blocks' "$f" || true
  if grep -qE 'ERROR SUMMARY: [1-9][0-9]* errors' "$f"; then
    vgfail=1
    grep -E 'definitely lost|Invalid |uninitialised|Syscall param| at 0x| by 0x' "$f" | head -100 || true
  fi
done

if [ "$FAIL" -ne 0 ] || [ "$vgfail" -ne 0 ]; then
  echo "VALGRIND_HOTPATH_FAIL smoke_fail=$FAIL vg_fail=$vgfail"
  exit 1
fi
echo "VALGRIND_HOTPATH_OK results in $OUT"
