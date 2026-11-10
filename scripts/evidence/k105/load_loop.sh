#!/bin/bash
# K105: the load beside d4_loop.sh: the fast suite (`make test`) at 8 jobs, repeated in a built tree copy until
# OUTROOT/STOP-TAG exists. usage: load_loop.sh TREE TAG [OUTROOT]. One line per cycle in OUTROOT/load-TAG.tsv.
TREE=$1; TAG=$2; ROOT=${3:-/tmp/efs-taskFK}
cd "$TREE" || exit 2
k=0
while [ ! -e "$ROOT/STOP-$TAG" ]; do
  k=$((k+1))
  EFS_TEST_TARGET=test EFS_TEST_JOBS=8 TMPDIR=${ROOT}-tmp/load-$TAG scripts/make_test_summary.sh > /dev/null 2>&1
  printf '%s\t%s\t%s\n' "$k" "$(head -1 summary.txt)" "$(date -u +%H:%M:%S)" >> "$ROOT/load-$TAG.tsv"
done
