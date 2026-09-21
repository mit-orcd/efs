#!/bin/bash
# Fresh-parent mkdir burst, repeated. One of nine concurrent mkdtemp+rmdir
# in a fresh directory ate the whole 10.35 s BUSY/STALE retry budget on
# Sep 21 (results/measure/20260921-203405-w8-root, sub burst 8/9). Each
# round: one new parent, 9 hosts mkdtemp+rmdir at once (12 s cap so the
# 10.35 s budget shows as EBUSY, not as a kill), then the server dir-op
# failure lines (raft-host: mkdir/rmdir/create/unlink ... rc=) and the
# client "exhausted 16 BUSY/STALE retries" lines since the start.
# ROUNDS (default 6). ~2 min.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
ROUNDS=${ROUNDS:-6}
mkout w8-parent-burst
preflight_or_die
srv_log_mark
declare -A CLI_MARK
for h in $ALL_CLIENTS; do CLI_MARK[$h]=$(ssh_ 10 "$h" 'wc -l < /tmp/efs/fuse.log'); done

ok_total=0; bad_total=0; slow_total=0
for r in $(seq 1 "$ROUNDS"); do
    P="$MNT/w8pb-$r-$$"
    ssh_ 15 fcstor007 "mkdir $P && echo PARENT_OK" > "$OUT/parent-$r.txt" 2>&1
    grep -q PARENT_OK "$OUT/parent-$r.txt" || { say "round $r: parent mkdir failed"; cat "$OUT/parent-$r.txt"; break; }
    for h in $ALL_CLIENTS; do
        ssh_ 25 "$h" "timeout -k 2 12 python3 -c 'import os,tempfile,time
t=time.time()
try:
    d=tempfile.mkdtemp(prefix=\"w8-\", dir=\"$P\"); t1=time.time()
    os.rmdir(d); print(\"OK mkdir=%.3f rmdir=%.3f\"%(t1-t,time.time()-t1))
except OSError as e:
    print(\"ERR %.3f %s\"%(time.time()-t,e))'; echo rc=\$?" > "$OUT/r$r-$h.txt" 2>&1 &
    done
    wait
    ok=0; bad=0; slow=0
    for h in $ALL_CLIENTS; do
        l=$(head -1 "$OUT/r$r-$h.txt")
        case "$l" in
            OK*) ok=$((ok+1)); m=$(echo "$l" | sed 's/.*mkdir=\([0-9.]*\).*/\1/'); awk -v m="$m" 'BEGIN{exit !(m>=0.5)}' && slow=$((slow+1));;
            *) bad=$((bad+1)); echo "  round $r $h: $l";;
        esac
    done
    say "round $r: ok=$ok bad=$bad slow(>=0.5s)=$slow"
    ok_total=$((ok_total+ok)); bad_total=$((bad_total+bad)); slow_total=$((slow_total+slow))
    ssh_ 15 fcstor007 "rmdir $P 2>&1 && echo PARENT_RM_OK || ls -la $P" >> "$OUT/parent-$r.txt" 2>&1
    grep -q PARENT_RM_OK "$OUT/parent-$r.txt" || { say "round $r: parent rmdir FAILED:"; tail -5 "$OUT/parent-$r.txt"; }
done

say "== server dir-op failure lines since start"
: > "$OUT/srv-dirop.txt"
for h in $SERVERS; do
    ssh_ 15 "$h" "tail -n +$(( ${SRV_MARK[$h]:-0} + 1 )) /tmp/efs/efsd.log | grep -E 'raft-host: (mkdir|rmdir|create|unlink)' | sed 's/^/$h /'" >> "$OUT/srv-dirop.txt"
done
wc -l < "$OUT/srv-dirop.txt"; head -40 "$OUT/srv-dirop.txt"
say "== client exhausted-retry lines since start"
: > "$OUT/cli-exhausted.txt"
for h in $ALL_CLIENTS; do
    ssh_ 10 "$h" "tail -n +$(( ${CLI_MARK[$h]:-0} + 1 )) /tmp/efs/fuse.log | grep -E 'inode-rpc: .*exhausted' | sed 's/^/$h /'" >> "$OUT/cli-exhausted.txt"
done
cat "$OUT/cli-exhausted.txt"
{
    echo "rounds=$ROUNDS ok=$ok_total bad=$bad_total slow=$slow_total"
    echo "server dir-op failure lines: $(wc -l < "$OUT/srv-dirop.txt")"
    echo "client exhausted lines: $(wc -l < "$OUT/cli-exhausted.txt")"
} | tee "$OUT/SUMMARY.txt"
echo DONE
