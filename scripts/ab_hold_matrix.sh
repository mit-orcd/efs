#!/bin/sh
# AB: h->mu hold/wait and commit progress, baseline vs candidate, diagnostics
# off/on, same host and build flags. Run from a directory holding two
# instrumented, built, non-Git source copies (see pump_logging_measure.py).
#   sh scripts/ab_hold_matrix.sh BASE_TREE NEW_TREE OUT [RUNS] [SECONDS]
# OUT must be new and under $HOME; TMPDIR must be under $HOME as well.
set -eu
base=$1 new=$2 out=$3 runs=${4:-3} secs=${5:-60}
mkdir "$out"
uptime > "$out/load-before.txt"
for r in $(seq 1 "$runs"); do
  for obs in 0 1; do
    for tree in base new; do
      src=$base; [ "$tree" = new ] && src=$new
      case=$tree-obs$obs-r$r
      (cd "$src" && python3 scripts/pump_logging_profile.py --work "$out/work-$case" \
        --output "$out/$case" --sink file --obs $obs --seconds "$secs" --threads 4 \
        --rate 5 --versioned > "$out/$case.stdout" 2>&1) || echo "$case FAILED" >> "$out/failures.txt"
      rm -rf "$out/work-$case"
    done
  done
done
uptime > "$out/load-after.txt"
python3 "$new/scripts/pump_logging_measure.py" summary "$out" > "$out/summary.json"
echo done > "$out/DONE"
