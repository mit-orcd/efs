#!/bin/bash
# Task DP: run tests/test_k38_protection_now.py RUNS times, one after another,
# beside LOADJOBS workers that each run the gate neighbours of that test
# (indexes 495-520 of a gate's timings.tsv, k38 itself excluded) in rotation.
# usage (from a built tree root: make test-bins tests/faults/efsd):
#   scripts/evidence/dp/k38_loaded_loop.sh OUT RUNS LOADJOBS TIMINGS_TSV
# Writes OUT/runs.tsv (run, seconds, rc, work dir), OUT/times.txt (the
# test's per-phase marks), OUT/run-N.log (output, UTC-stamped) and
# OUT/load.tsv (each neighbour run). A run over 30 s or failing keeps its
# work dir (daemon logs) as OUT/work-N; the others are removed.
# TMPDIR and EFS_FUSE_PREFIX come from the caller.
out=$1; runs=$2; jobs=$3; ts=$4
[ -n "$ts" ] && [ -f "$ts" ] || { echo "usage: $0 OUT RUNS LOADJOBS TIMINGS_TSV" >&2; exit 2; }
mkdir -p "$out"
[ -n "$EFS_FUSE_PREFIX" ] && export LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64
awk -F'\t' '$1>=495 && $1<=520 && $4 !~ /protection_now/ {print $4}' "$ts" > "$out/neighbours.txt"
touch "$out/load.on"
loadworker() { # $1 = worker id
  local k=$1 n=0
  mapfile -t cmds < "$out/neighbours.txt"
  while [ -e "$out/load.on" ]; do
    c=${cmds[$(( (k*3 + n) % ${#cmds[@]} ))]}; n=$((n+1))
    s=$(date +%s.%N); bash -c "$c" > "$out/load-$k-$n.log" 2>&1; rc=$?
    printf '%s\t%s\t%.1f\t%s\t%s\n' "$(date -u +%FT%TZ)" "$k" "$(echo "$(date +%s.%N) - $s" | bc)" "$rc" "$c" >> "$out/load.tsv"
    [ $rc = 0 ] && rm -f "$out/load-$k-$n.log"
  done
}
for k in $(seq 1 "$jobs"); do loadworker "$k" & done
for r in $(seq 1 "$runs"); do
  s=$(date +%s.%N)
  EFS_K38_KEEP=1 EFS_K38_TIMES="$out/times.txt" python3 -u tests/test_k38_protection_now.py 2>&1 \
    | while IFS= read -r l; do printf '%s %s\n' "$(date -u +%T.%3N)" "$l"; done > "$out/run-$r.log"
  rc=${PIPESTATUS[0]}
  d=$(echo "$(date +%s.%N) - $s" | bc)
  w=$(grep -o 'kept .*' "$out/run-$r.log" | cut -d' ' -f2)
  printf '%s\t%.1f\t%s\t%s\n' "$r" "$d" "$rc" "$w" >> "$out/runs.tsv"
  if [ -n "$w" ]; then
    if (( $(echo "$d > 30" | bc) )) || [ "$rc" != 0 ]; then mv "$w" "$out/work-$r"; else rm -rf "$w"; fi
  fi
done
rm -f "$out/load.on"; wait
echo done > "$out/DONE"
