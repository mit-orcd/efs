#!/bin/bash
#SBATCH --job-name=efs-valgrind
#SBATCH --partition=mit_normal
#SBATCH --time=01:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/valgrind-%j.err

# Build with debug symbols and run the non-FUSE tests under Valgrind.
# FUSE (test_rw.sh) is skipped: kernel FUSE + Valgrind is unreliable/slow.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
OUT="$SHARED/valgrind/${SLURM_JOB_ID}"

mkdir -p "$SHARED/logs" "$OUT"
rm -rf "$SCRATCH"
mkdir -p "$SCRATCH"

cleanup() {
    rm -rf "$SCRATCH"
}
trap cleanup EXIT TERM INT

cd "$REPO"

echo "=== host=$(hostname) job=$SLURM_JOB_ID $(date) ==="
command -v valgrind
valgrind --version
# Hide AVX-512 from glibc IFUNCs under Valgrind.
export GLIBC_TUNABLES="glibc.cpu.hwcaps=-AVX512F,-AVX512DQ,-AVX512VL,-AVX512BW,-AVX512CD"


echo "=== building with -O1 -g ==="
# Clean project objects only — do not wipe deps/libfuse (configure + headers).
rm -f src/common/*.o src/client/*.o src/server/*.o src/mgmt/*.o src/query/*.o
rm -f deps/blake3/*.o libefs.a efsd efs-fuse efs-mgmt efs-query
rm -f tests/test_erasure tests/test_placement tests/test_local_ec \
      tests/test_numa_locality tests/test_integration \
      tests/test_quota tests/test_migrate tests/test_directio \
      tests/test_rejoin tests/test_query tests/test_list_exports

# Ensure libfuse is present before compiling efs-fuse (parallel make race).
make -j"${SLURM_CPUS_PER_TASK}" deps/libfuse/lib/.libs/libfuse.a

# Command-line CFLAGS overrides Makefile CFLAGS, so include FUSE flags here.
make -j"${SLURM_CPUS_PER_TASK}" \
  CFLAGS='-O1 -g -fno-omit-frame-pointer -std=c99 -Wall -Wextra -D_GNU_SOURCE -DBLAKE3_NO_AVX512 -Wno-stringop-truncation -Wno-format-truncation -Ideps/libfuse/include -D_FILE_OFFSET_BITS=64'

# definite+indirect only as failures; "still reachable" at exit is often
# process-lifetime pools (TCP conn fds, pthread mutexes) and is summarized.
VG=(
  valgrind
  --tool=memcheck
  --leak-check=full
  --show-leak-kinds=definite,indirect
  --errors-for-leak-kinds=definite,indirect
  --track-origins=yes
  --error-exitcode=42
  --num-callers=30
  --trace-children=yes
  --child-silent-after-fork=yes
  --suppressions=/dev/null
)

VG_COMMON=(
  --tool=memcheck
  --leak-check=full
  --show-leak-kinds=definite,indirect
  --errors-for-leak-kinds=definite,indirect
  --track-origins=yes
  --error-exitcode=42
  --num-callers=30
  --trace-children=yes
  --child-silent-after-fork=yes
)
if [ -f "$REPO/slurm-jobs/valgrind.supp" ]; then
  VG_COMMON+=(--suppressions="$REPO/slurm-jobs/valgrind.supp")
fi

TESTS=(
  tests/test_erasure
  tests/test_placement
  tests/test_local_ec
  tests/test_numa_locality
  tests/test_integration
  tests/test_quota
  tests/test_migrate
  tests/test_directio
  tests/test_rejoin
  tests/test_query
  tests/test_list_exports
)

fail=0
summary="$OUT/SUMMARY.txt"
: > "$summary"

for t in "${TESTS[@]}"; do
  name=$(basename "$t")
  logdir="$OUT/$name"
  mkdir -p "$logdir"
  echo "=== $name ===" | tee -a "$summary"
  # %p: one log per process so children do not clobber parent reports.
  if TMPDIR="$SCRATCH" valgrind "${VG_COMMON[@]}" \
      --log-file="$logdir/%p.txt" "./$t"; then
    echo "OK $name" | tee -a "$summary"
  else
    rc=$?
    echo "FAIL $name rc=$rc (see $logdir/)" | tee -a "$summary"
    fail=1
  fi
  # Pull summaries from every process log.
  grep -Eh 'ERROR SUMMARY|definitely lost|indirectly lost|Invalid |uninitialised|Syscall param' \
    "$logdir"/*.txt 2>/dev/null | tee -a "$summary" || true

  # Parent exit code ignores traced children; fail if any process log has errors.
  while IFS= read -r line; do
    if [[ "$line" =~ ERROR\ SUMMARY:\ ([0-9]+)\ errors ]] && [[ "${BASH_REMATCH[1]}" -gt 0 ]]; then
      echo "VALGRIND_ERRORS in $name: $line" | tee -a "$summary"
      fail=1
    fi
  done < <(grep -h 'ERROR SUMMARY:' "$logdir"/*.txt 2>/dev/null || true)

  echo | tee -a "$summary"
done

echo "=== done fail=$fail results in $OUT ===" | tee -a "$summary"
exit "$fail"
