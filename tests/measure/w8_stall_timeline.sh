#!/bin/bash
# w8_stall_timeline.sh — where is the >15 s stall inside the 9-host posix
# suite? Trivial tests (err_stat_nonexistent, 3 ops) time out at 15 s on
# every host at once, so it is a cluster-wide window, not per-op cost.
#
# Runs the 9-host suite detached and, for its whole duration, samples at
# 1 Hz: (a) on fcstor003, raft term/leader/commit of both groups;
# (b) on fcstor007 (under suite load itself), wall time of one stat of the
# mount root and one mkdir+rmdir of a unique root child. Output:
#   raft.tsv  ts group role leader term commit applied
#   ops.tsv   ts stat_s mkdir_s rmdir_s rc
# then a merged view of every second where any op took >1 s or a term
# moved. ~150 s; run via efs-bg.sh.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
ALL_CLIENTS="fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015"
MNT=/tmp/efs-mount
DUR=${DUR:-140}
OUT="$REPO/results/measure/$(date +%Y%m%d-%H%M%S)-w8-stall-timeline"
mkdir -p "$OUT"
say() { echo "[w8_stall_timeline] $*"; }
ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$S" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }
say "results -> $OUT"

bash "$REPO/tests/preflight.sh" > "$OUT/preflight.txt" 2>&1 || { cat "$OUT/preflight.txt"; exit 1; }
grep PREFLIGHT_OK "$OUT/preflight.txt"

# Samplers first (they start within a second), then the suite.
ssh_ $((DUR + 30)) fcstor003 'cd /tmp/efs; end=$((SECONDS + '"$DUR"')); while [ $SECONDS -lt $end ]; do
    ts=$(date +%s.%N | cut -c1-14)
    ./efs-mgmt raft-status 172.16.223.57:19810 2>/dev/null | awk -v ts=$ts "/group 0/ {gsub(/[a-z_]+=/,\"\"); print ts, \"g0\", \$4, \$5, \$6, \$7, \$8}"
    ./efs-mgmt raft-status 172.16.223.58:19810 2>/dev/null | awk -v ts=$ts "/group 2/ {gsub(/[a-z_]+=/,\"\"); print ts, \"g2\", \$4, \$5, \$6, \$7, \$8}"
    sleep 1; done' > "$OUT/raft.tsv" 2>&1 &
RAFT_PID=$!
ssh_ $((DUR + 60)) fcstor007 'end=$((SECONDS + '"$DUR"')); i=0; while [ $SECONDS -lt $end ]; do
    i=$((i+1)); ts=$(date +%s.%N | cut -c1-14)
    t0=$(date +%s.%N); timeout 30 stat '"$MNT"'/ >/dev/null 2>&1; rc1=$?; t1=$(date +%s.%N)
    d='"$MNT"'/stall-probe-$$-$i
    timeout 30 mkdir $d 2>/dev/null; rc2=$?; t2=$(date +%s.%N)
    timeout 30 rmdir $d 2>/dev/null; rc3=$?; t3=$(date +%s.%N)
    awk -v ts=$ts -v a=$t0 -v b=$t1 -v c=$t2 -v e=$t3 -v r="$rc1/$rc2/$rc3" "BEGIN{printf \"%s %.3f %.3f %.3f %s\n\", ts, b-a, c-b, e-c, r}"
    sleep 1; done' > "$OUT/ops.tsv" 2>&1 &
OPS_PID=$!

say "== start the 9-host suite (detached)"
T0=$(date +%s)
(cd "$REPO" && EFS_TRANSPORT=tcp bash tests/run_tests.sh posix --parallel \
    $(for h in $ALL_CLIENTS; do printf '%s.ib ' "$h"; done) > "$OUT/suite.log" 2>&1) &
SUITE_PID=$!
wait $SUITE_PID
say "suite done after $(( $(date +%s) - T0 )) s: $(grep -o 'results in .*' "$OUT/suite.log" | tail -1)"
wait $RAFT_PID $OPS_PID

{
    echo "suite $(grep -o 'results in .*' "$OUT/suite.log" | tail -1); suite start epoch $T0"
    for f in "$REPO"/results/posix/$(grep -o 'results in .*' "$OUT/suite.log" | tail -1 | awk '{print $3}' | xargs basename)/efs-*.tsv; do
        echo "$(basename "$f") $(grep '^# summary' "$f")"
    done
    echo
    echo "-- term/leader changes (raft.tsv):"
    awk '{k=$2; v=$4" "$5; if (v != last[k]) {print $1, k, $3, v, "commit="$6; last[k]=v}}' "$OUT/raft.tsv"
    echo
    echo "-- seconds where stat/mkdir/rmdir took >1 s or rc!=0/0/0 (ops.tsv):"
    awk '$2>1 || $3>1 || $4>1 || $5!="0/0/0"' "$OUT/ops.tsv"
    echo
    echo "-- ops latency percentiles (s): stat / mkdir / rmdir"
    for c in 2 3 4; do awk -v c=$c '{print $c}' "$OUT/ops.tsv" | sort -n | awk '{a[NR]=$1} END{printf "  n=%d p50=%.3f p90=%.3f p99=%.3f max=%.3f\n", NR, a[int(NR*.5)+1], a[int(NR*.9)+1], a[int(NR*.99)+1], a[NR]}'; done
    echo
    echo "-- commit rate per group (entries/s over the run):"
    awk '{if (!($2 in f)) {f[$2]=$6; t[$2]=$1} l[$2]=$6; u[$2]=$1} END{for (k in f) printf "  %s %.0f entries/s over %.0f s\n", k, (l[k]-f[k])/(u[k]-t[k]+0.001), u[k]-t[k]}' "$OUT/raft.tsv"
} | tee "$OUT/REPORT.txt"
say "DONE"
