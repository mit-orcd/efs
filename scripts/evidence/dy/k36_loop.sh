#!/bin/sh
# Solo loop of tests/test_k36_recovery.py under the line-gap tracer.
# Usage: k36_loop.sh TREE OUT RUNS. Each run leaves OUT/runNN.{out,err,rc}
# OUT/runNN.load (load average, efsd count at start)
# and OUT/runNN-logs/ (daemon and mount logs only; stores are deleted).
# Needs TMPDIR set; the tree built with `make test-bins tests/faults/efsd`.
set -u
tree=$1 out=$2 runs=$3
mkdir -p "$out"
cd "$tree" || exit 2
i=1
while [ "$i" -le "$runs" ]; do
    n=$(printf %02d "$i")
    # Solo means solo: the load and any other efsd at start go with the run.
    { cat /proc/loadavg; pgrep -c efsd; } > "$out/run$n.load"
    EFS_K36_KEEP=1 python3 scripts/evidence/dy/k36_trace.py 1.0 \
        > "$out/run$n.out" 2> "$out/run$n.err"
    echo $? > "$out/run$n.rc"
    work=$(sed -n 's/^kept //p' "$out/run$n.out" | tail -1)
    if [ -n "$work" ] && [ -d "$work" ]; then
        mkdir -p "$out/run$n-logs"
        (cd "$work" && find . -name '*.log' -type f | while read -r f; do
            mkdir -p "$out/run$n-logs/$(dirname "$f")"; cp "$f" "$out/run$n-logs/$f"; done)
        rm -rf "$work"
    fi
    i=$((i + 1))
done
