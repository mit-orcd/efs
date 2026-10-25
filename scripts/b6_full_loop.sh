#!/bin/bash
# b6_full_loop.sh TREE OUTDIR RUNS: build TREE (efsd, efs-fuse, efs-mgmt and
# the EFS_FAULTS efsd) and run `EFS_B6_MODES=full tests/test_b6_unavailable.py`
# RUNS times, for the packer UNINDEX race (docs/status/
# pack-unindex-race-20261024.md). Linux with /dev/fuse and fusermount3; the
# test picks free ports and keeps its stores under TMPDIR (default
# $HOME/efs-b6loop-tmp). OUTDIR gets summary.txt (one `runN rc= secs=` line
# per run, the run's verdict line, then DONE), runN/stdout.log,
# runN/b6-result.json and SHA256SUMS; keep it outside Git.
# EFS_PACK_DBG=1 in the environment adds the packer's drop traces to the
# daemon logs (they stay in the test's private TMPDIR, removed with it).
# EFS_B6_LOOP_NOBUILD=1 skips the build (a second loop on the same tree).
set -u
[ $# -eq 3 ] || { echo "usage: $0 TREE OUTDIR RUNS" >&2; exit 2; }
tree=$1 out=$2 runs=$3
mkdir -p "$out"
cd "$tree" || exit 2
export TMPDIR=${TMPDIR:-$HOME/efs-b6loop-tmp}
mkdir -p "$TMPDIR"
if [ -z "${EFS_B6_LOOP_NOBUILD:-}" ]; then
    make -j"$(nproc)" all tests/faults/efsd > "$out/build.log" 2>&1 || { echo "BUILD FAIL" > "$out/summary.txt"; exit 1; }
fi
fails=0
for i in $(seq 1 "$runs"); do
    mkdir -p "$out/run$i"
    s=$(date +%s)
    EFS_B6_MODES=full EFS_B6_ARTIFACTS="$out/run$i" timeout 3600 \
        python3 tests/test_b6_unavailable.py > "$out/run$i/stdout.log" 2>&1
    rc=$?
    [ $rc -eq 0 ] || fails=$((fails + 1))
    echo "run$i rc=$rc secs=$(( $(date +%s) - s )) $(grep -E 'PASS|FAIL|packed [0-9]+ of' "$out/run$i/stdout.log" | tail -1)" >> "$out/summary.txt"
done
echo DONE >> "$out/summary.txt"
(cd "$out" && find . -type f ! -name SHA256SUMS -print0 | xargs -0 sha256sum > SHA256SUMS)
[ $fails -eq 0 ]
