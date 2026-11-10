#!/bin/bash
# Task FF (K104): forced_loop.sh TREE PAR ROUNDS OUT [SCRATCH]
# PAR copies per round of forced_election.py on TREE (built), each with its
# own TMPDIR under SCRATCH (default $HOME/efs-taskFF/tmp) and, through
# port_range.py, its own 12-port slot (PAR <= 8; 18700-18799 on intel1).
# When a run's first halted listing is 5 s overdue (listing-0010.json older
# than 5 s, no listing-0011.json), pstack of its efs-fuse and efsd goes to
# <fixture>/pstack.txt. One line per run in OUT/runs.tsv:
# tree round slot rc elapsed_s load1 time. Passing fixtures without a pstack
# are deleted; failed or captured ones are kept for the evidence copy.
t=$1 par=$2 rounds=$3 out=$4 S=${5:-$HOME/efs-taskFF/tmp}
FF=$(cd "$(dirname "$0")" && pwd); mkdir -p "$out"
watch() { d=$1; end=$((SECONDS+400)); while [ $SECONDS -lt $end ]; do
  ev=$(ls -d "$d"/efs-ao-live-*/evidence 2>/dev/null | head -1)
  if [ -n "$ev" ] && [ -f "$ev/listing-0010.json" ] && [ ! -f "$ev/listing-0011.json" ] &&
     [ $(( $(date +%s) - $(stat -c %Y "$ev/listing-0010.json") )) -ge 5 ]; then
    fx=${ev%/evidence}
    for p in $(pgrep -u "$(id -un)" -f "$fx/work/cluster"); do
      { echo "== pid $p $(tr '\0' ' ' < /proc/$p/cmdline | cut -c1-160)"; date -u +%T.%N; pstack $p; } >> "$fx/pstack.txt" 2>&1
    done; return; fi; sleep 1; done; }
for r in $(seq 1 "$rounds"); do
  for s in $(seq 0 $((par-1))); do
    ( d=$S/forced-$(basename "$t")/r$r-s$s; mkdir -p "$d"; cd "$t"; watch "$d" & w=$!
      l0=$(cut -d' ' -f1 /proc/loadavg); t0=$(date +%s.%N)
      TMPDIR=$d python3 "$FF/port_range.py" "$t" $s "$FF/forced_election.py" "$t" > "$d/out.txt" 2>&1; rc=$?
      kill $w 2>/dev/null
      printf "%s\t%s\t%s\t%s\t%.1f\t%s\t%s\n" "$(basename "$t")" $r $s $rc "$(echo "$(date +%s.%N) - $t0" | bc)" "$l0" "$(date -u +%TZ)" >> "$out/runs.tsv"
      if [ $rc = 0 ] && ! ls "$d"/*/pstack.txt >/dev/null 2>&1; then rm -rf "$d"; fi ) &
  done
  wait
done
echo done >> "$out/runs.tsv.log"
