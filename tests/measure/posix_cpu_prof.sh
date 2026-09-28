#!/bin/bash
# posix_cpu_prof.sh — cpu-clock stacks and a syscall summary while one
# posix jobs=1 suite runs on one client.
#
# Does not remount and does not change EFS_TRANSPORT. The daemons must
# already be on the transport you want to label. Two suite passes: the
# first overlaps perf record on the client efs-fuse and both group
# leaders; the second overlaps `perf trace -s` on the same three
# processes. Attach only; never start a daemon under perf.
#
# Usage (from node9901 via efs-bg.sh, ~3 min):
#   LABEL=rdma bash tests/measure/posix_cpu_prof.sh fcstor007
#   LABEL=tcp  bash tests/measure/posix_cpu_prof.sh fcstor007
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
LABEL="${LABEL:-untagged}"
HOST="${1:-fcstor007}"
SECS="${SECS:-55}"
mkout "posix-prof-$LABEL"
preflight_or_die

trace_daemon() {
    local h=$1 p=$2 s=$3 b=$4
    ssh_ $((s + 45)) "$h" "pid=\$(pgrep -x $p | head -1); [ -n \"\$pid\" ] || { echo no-$p; exit 1; }
        perf trace -p \$pid -s -- sleep $s" > "$OUT/trace-$b-$h.txt" 2>&1
    say "trace $p on $h -> $OUT/trace-$b-$h.txt ($(wc -l < "$OUT/trace-$b-$h.txt") lines)"
}

one_suite() { # tag
    local tag=$1
    say "posix jobs=1 ($tag) on $HOST transport=$LABEL"
    EFS_TRANSPORT="${EFS_TRANSPORT:-}" POSIX_JOBS=1 \
        bash "$REPO/tests/run_tests.sh" posix "$HOST.ib" \
        > "$OUT/posix-$tag.log" 2>&1
    local rc=$?
    grep -E 'POSIX suite:|EFS BUGS|not an efs-fuse' "$OUT/posix-$tag.log" | tail -8
    return $rc
}

l0=$(leader_host 0 || true)
l2=$(leader_host 2 || true)
say "leaders g0=${l0:-?} g2=${l2:-?}"
printf 'g0=%s g2=%s label=%s\n' "$l0" "$l2" "$LABEL" > "$OUT/leaders.txt"
[ -n "$l0" ] && [ -n "$l2" ] || { say "no leader"; exit 1; }

say "pass 1: cpu-clock during the suite (${SECS}s, start after XFS baseline)"
( sleep 16; perf_daemon "$HOST" efs-fuse "$SECS" "cpu-fuse" ) &
( sleep 16; perf_daemon "$l0" efsd "$SECS" "cpu-efsd-g0" ) &
( sleep 16; perf_daemon "$l2" efsd "$SECS" "cpu-efsd-g2" ) &
one_suite cpu
cpu_rc=$?
wait

say "pass 2: perf trace -s during a second suite"
( sleep 16; trace_daemon "$HOST" efs-fuse "$SECS" "fuse" ) &
( sleep 16; trace_daemon "$l0" efsd "$SECS" "efsd-g0" ) &
( sleep 16; trace_daemon "$l2" efsd "$SECS" "efsd-g2" ) &
one_suite trace
tr_rc=$?
wait

{
    echo "posix cpu+trace profile label=$LABEL build=$BUILD host=$HOST"
    echo "leaders g0=$l0 g2=$l2  record_s=$SECS"
    echo "suite cpu pass rc=$cpu_rc; trace pass rc=$tr_rc"
    echo "(rc=1 with 0 EFS-bugs is compare exit 2: mmap SKIP on 5.14)"
    echo
    echo "== suite lines =="
    grep -E 'POSIX suite:|EFS BUGS' "$OUT"/posix-*.log
    echo
    echo "== cpu-clock top (fuse) =="
    head -40 "$OUT/perf-cpu-fuse-$HOST.txt" 2>/dev/null || echo "(missing)"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
[ "$cpu_rc" = 0 ] || [ "$cpu_rc" = 1 ]
exit $?
