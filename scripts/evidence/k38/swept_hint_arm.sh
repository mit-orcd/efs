#!/bin/bash
# Task DG (K38 image-base stall, docs/status/2026-11/k38-acceptance-20261105.md):
# N invocations of tests/test_k38_swept_hint_live.py from TREE (two rounds each).
# Usage: swept_hint_arm.sh ARM TREE N [OUT]   (OUT default /tmp/efs-taskDG/arms/ARM)
# Writes OUT/hashes.txt, OUT/run-I.log and OUT/summary.txt (rc and seconds per run),
# and copies a kept (failing) first run's daemon and mount logs to OUT/run1-logs.
# Export LD_LIBRARY_PATH inside a screen's command: screen drops it (setgid).
arm=$1 tree=$2 n=$3 out=${4:-/tmp/efs-taskDG/arms/$1}
mkdir -p "$out"; cd "$tree" || exit 2
sha256sum efsd efs-fuse tests/faults/efsd > "$out/hashes.txt"
base=${TMPDIR:-/tmp}
for i in $(seq 1 "$n"); do
  export TMPDIR=$base/$arm-$i; mkdir -p "$TMPDIR"
  t=$(date +%s.%N)
  python3 tests/test_k38_swept_hint_live.py > "$out/run-$i.log" 2>&1; rc=$?
  echo "run=$i rc=$rc secs=$(echo "$(date +%s.%N) - $t" | bc)" >> "$out/summary.txt"
  if [ "$i" = 1 ] && [ ! -d "$out/run1-logs" ]; then
    k=$(ls -d "$TMPDIR"/efs-k38s-*/k38s 2>/dev/null | head -1)
    [ -n "$k" ] && mkdir -p "$out/run1-logs" && cp "$k"/*.log "$out/run1-logs/"
  fi
  rm -rf "$TMPDIR"
done
echo done >> "$out/summary.txt"
