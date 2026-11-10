#!/bin/bash
# K110 (Task FH): a series of `append_failover.py --failover kill` runs on private ports.
# usage: run_series.sh <first> <last> <tag> <rel> [extra append_failover.py args...]
#   <rel>  directory holding a built efsd, efs-fuse and efs-mgmt (the tree root)
# env:   WORK (default /tmp/efs-k110) holds the run roots and the kept logs (out/).
# Each run: 8,000 records, 4 writers, one shared file, the current leader of group
# 0 or 2 SIGKILLed about every 12 s and restarted after 5 s, one final fsync per
# writer, plus the observation-only --post-fsync (two more fsyncs, outside the verdict).
# Run it from the tree root. The run roots (stores) are kept; delete them when done.
set -u
f=$1; l=$2; t=$3; rel=$4; shift 4
WORK=${WORK:-/tmp/efs-k110}; OUT=$WORK/out; mkdir -p "$OUT" "$WORK/tmp"
export TMPDIR=$WORK/tmp
for i in $(seq "$f" "$l"); do
  R=$WORK/runs/$t-$i; rm -rf "$R"; s=$(date +%s)
  python3 tests/live/append_failover.py --rel "$rel" --root "$R" --port "${PORT:-38100}" \
    --records 8000 --writers 4 --failover kill --interval 4 --first-event 3 --down 5 \
    --post-fsync --label "$t-$i" "$@" > "$OUT/$t-$i.out" 2>&1
  echo "run $t-$i rc=$? wall=$(( $(date +%s)-s ))s" >> "$OUT/summary.txt"
  cp "$R/report.json" "$OUT/$t-$i.report.json" 2>/dev/null
  mkdir -p "$OUT/$t-$i.logs"; cp "$R"/logs/* "$OUT/$t-$i.logs/" 2>/dev/null
done
echo "LOOP $t $f-$l done" >> "$OUT/summary.txt"
