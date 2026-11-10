#!/bin/bash
# K105: run tests/test_d4_progress.py N times in a built tree and keep a table.
# usage: d4_loop.sh TREE TAG N [OUTROOT]   (OUTROOT default /tmp/efs-taskFK, TMPDIR default $OUTROOT-tmp)
# Per run: OUTROOT/out-TAG/run-I.log (test output), runs.tsv (run, rc, seconds, 1-min load at start, UTC end),
# and for a failed run the kept work directory (listed in failures.txt). A passing run's directory is removed.
# Stops early when OUTROOT/STOP-TAG exists. Raw output stays outside Git.
TREE=$1; TAG=$2; N=$3; ROOT=${4:-/tmp/efs-taskFK}
OUT=$ROOT/out-$TAG; mkdir -p "$OUT"
for i in $(seq 1 "$N"); do
  [ -e "$ROOT/STOP-$TAG" ] && break
  T=${ROOT}-tmp/$TAG-$i; mkdir -p "$T"
  s=$(date +%s); l=$(cut -d' ' -f1 /proc/loadavg)
  (cd "$TREE" && TMPDIR=$T python3 tests/test_d4_progress.py > "$OUT/run-$i.log" 2>&1); rc=$?
  e=$(date +%s)
  printf '%s\t%s\t%s\t%s\t%s\n' "$i" "$rc" $((e-s)) "$l" "$(date -u +%H:%M:%S)" >> "$OUT/runs.tsv"
  if [ "$rc" -ne 0 ]; then echo "FAIL $i" >> "$OUT/failures.txt"; else ~/bin/edelete --force --delete "$T" > /dev/null 2>&1; fi
done
echo done >> "$OUT/runs.tsv"
