#!/bin/bash
# dd_cpu_prof.sh — cpu-clock while 9 clients each write 8 GiB with fsync.
#
# Same shape as tests/measure/dd_wall.sh for one client count: own file,
# non-zero source, flush in the clock, FUSE_OK required. Does not remount
# and does not change EFS_TRANSPORT. Attach perf to the running efs-fuse
# on fcstor007 and both raft leaders for the whole dd. No perf trace.
#
# Usage (from node9901 via efs-bg.sh, ~2 min):
#   LABEL=r1 EFS_TRANSPORT=rdma NCLIENTS=9 bash tests/measure/dd_cpu_prof.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
LABEL="${LABEL:-untagged}"
NCLIENTS="${NCLIENTS:-9}"
SECS="${SECS:-70}"
mkout "dd-prof-$LABEL"
preflight_or_die

hosts=$(echo $ALL_CLIENTS | tr ' ' '\n' | head -"$NCLIENTS" | tr '\n' ' ')
say "dd ${NCLIENTS} client(s): $hosts transport=${EFS_TRANSPORT:-keep}"
for h in $hosts; do
    line=$(ssh_ 15 "$h" "findmnt -no FSTYPE $MNT; timeout 2 stat $MNT >/dev/null && echo MOUNT_OK; grep -c 'RDMA transport up' /tmp/efs/fuse.log || true")
    echo "$h $line" | tee -a "$OUT/mounts.txt"
    echo "$line" | grep -q fuse.efs-fuse || { say "NOT_FUSE $h"; exit 1; }
    echo "$line" | grep -q MOUNT_OK || { say "MOUNT_BAD $h"; exit 1; }
done

l0=$(leader_host 0 || true)
l2=$(leader_host 2 || true)
say "leaders g0=${l0:-?} g2=${l2:-?}"
printf 'g0=%s g2=%s label=%s n=%s\n' "$l0" "$l2" "$LABEL" "$NCLIENTS" > "$OUT/leaders.txt"
[ -n "$l0" ] && [ -n "$l2" ] || { say "no leader"; exit 1; }

for h in $hosts; do ensure_src "$h" > "$OUT/src-$h.txt" & done
wait

srv_log_mark
( sleep 2; perf_daemon fcstor007 efs-fuse "$SECS" "cpu-fuse" ) &
( sleep 2; perf_daemon "$l0" efsd "$SECS" "cpu-efsd-g0" ) &
( sleep 2; perf_daemon "$l2" efsd "$SECS" "cpu-efsd-g2" ) &

t0=$(date +%s.%N)
for h in $hosts; do
    ssh_ 150 "$h" "fst=\$(findmnt -no FSTYPE $MNT 2>/dev/null); echo FUSE_CHECK fstype=\$fst
        [ \"\$fst\" = fuse.efs-fuse ] || { echo NOT_FUSE; exit 1; }; echo FUSE_OK
        rm -f $MNT/measure/dd-$h.bin; mkdir -p $MNT/measure
        TIMEFORMAT='WALL %R'; { time dd if=/tmp/src8g of=$MNT/measure/dd-$h.bin bs=1M conv=fsync status=none; } 2>&1
        ls -l $MNT/measure/dd-$h.bin" > "$OUT/dd-$h.txt" &
done
wait
pw=$(echo "$(date +%s.%N) - $t0" | bc)

walls=""; sumr=0; bad=0
for h in $hosts; do
    grep -q FUSE_OK "$OUT/dd-$h.txt" || { say "FAIL: $h NOT_FUSE"; bad=1; }
    grep -qE 'TIMEOUT|fsync failed|Input/output error' "$OUT/dd-$h.txt" && { say "FAIL: $h EIO or timeout"; bad=1; }
    grep -q 8589934592 "$OUT/dd-$h.txt" || { say "FAIL: $h file is not 8 GiB"; bad=1; }
    w=$(grep -o 'WALL [0-9.]*' "$OUT/dd-$h.txt" | cut -d' ' -f2)
    walls="$walls $w"
    [ -n "$w" ] && sumr=$(echo "scale=1; $sumr + 8192 / $w" | bc)
done
maxw=$(echo $walls | tr ' ' '\n' | sort -n | tail -1)
minw=$(echo $walls | tr ' ' '\n' | sort -n | head -1)
agg=$(echo "scale=1; $NCLIENTS * 8192 / ${maxw:-$pw}" | bc)
rs=$(srv_report_split "$OUT/report-split.txt")
say "$rs"

for h in $hosts; do ssh_ 30 "$h" "rm -f $MNT/measure/dd-$h.bin" >/dev/null 2>&1 & done
wait

{
    echo "dd cpu profile label=$LABEL build=$BUILD n=$NCLIENTS"
    echo "leaders g0=$l0 g2=$l2 record_s=$SECS"
    echo "ssh_wall_s=$pw slowest_s=${maxw:-?} fastest_s=${minw:-?}"
    if [ "$bad" = 0 ]; then
        echo "aggregate_MiB_s=$agg  (n * 8192 / slowest dd wall)"
    else
        echo "aggregate_MiB_s=INVALID"
    fi
    echo "per_client_walls:$walls"
    echo "$rs"
    echo
    echo "== fuse =="
    head -30 "$OUT/perf-cpu-fuse-fcstor007.txt" 2>/dev/null || echo "(missing)"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
[ "$bad" = 0 ]
