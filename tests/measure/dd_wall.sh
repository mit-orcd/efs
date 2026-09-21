#!/bin/bash
# dd_wall.sh — W4: the multi-client write wall. 8 GiB dd+fsync per client,
# own file each, from 1 / 4 / 9 clients at once; flush is in the clock.
# Sep 18 numbers: 639 / 251 / 202 MiB/s aggregate (3.8 / 0.46 / 0.37 % of
# the 16.7 / 44 / 44 GB/s ceilings) — writes SHARE a ceiling and more
# clients make it worse. This script re-measures that on the current build
# and, with PERF=1, profiles the group-0 leader's efsd and fcstor007's
# efs-fuse for 20 s in the middle of the 4-client run (off-CPU wait is the
# expected shape: the wall is the fsync/REPORT tail, not hashing).
#
# Usage (from node9901 via efs-bg.sh; ~8 min for 1+4+9):
#   bash tests/measure/dd_wall.sh
#   NCLIENTS="1 4" PERF=1 bash tests/measure/dd_wall.sh
#
# Every client is cold-remounted first and must show FUSE_OK (a dead
# efs-fuse leaves a plain dir and dd writes to local disk at GB/s — that
# number is fiction). Hand the user: the aggregate MiB/s per client count,
# per-client walls (spread = fairness), and the two perf reports.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
NCLIENTS="${NCLIENTS:-1 4 9}"
PERF="${PERF:-0}"
mkout dd-wall
preflight_or_die

printf 'clients\tssh_wall_s\tagg_mibs\tper_client_min_max_s\tsum_rates_mibs\tpct_of_ceiling\n' > "$OUT/table.tsv"
for n in $NCLIENTS; do
    hosts=$(echo $ALL_CLIENTS | tr ' ' '\n' | head -"$n" | tr '\n' ' ')
    say "== $n client(s): $hosts"
    remount_clients "$hosts" "" > "$OUT/remount-$n.txt" || { cat "$OUT/remount-$n.txt"; exit 1; }
    for h in $hosts; do ensure_src "$h" > /dev/null & done; wait
    if [ "$PERF" = 1 ] && [ "$n" -ge 4 ]; then
        l0=$(leader_host 0)
        ( sleep 20; perf_daemon "$l0" efsd 20 "ddwall-$n-efsd" ) &
        ( sleep 20; perf_daemon fcstor007 efs-fuse 20 "ddwall-$n-fuse" ) &
    fi
    srv_log_mark
    t0=$(date +%s.%N)
    for h in $hosts; do
        ssh_ 400 "$h" "fst=\$(findmnt -no FSTYPE $MNT 2>/dev/null); echo FUSE_CHECK fstype=\$fst
            [ \"\$fst\" = fuse.efs-fuse ] || { echo NOT_FUSE; exit 1; }; echo FUSE_OK
            rm -f $MNT/measure/dd-$h.bin; mkdir -p $MNT/measure
            TIMEFORMAT='WALL %R'; { time dd if=/tmp/src8g of=$MNT/measure/dd-$h.bin bs=1M conv=fsync status=none; } 2>&1
            ls -l $MNT/measure/dd-$h.bin" > "$OUT/dd-$n-$h.txt" &
    done
    wait
    pw=$(echo "$(date +%s.%N) - $t0" | bc)
    walls=""; sumr=0; bad=0
    for h in $hosts; do
        grep -q FUSE_OK "$OUT/dd-$n-$h.txt" || { say "FAIL: $h NOT_FUSE — number discarded"; bad=1; }
        w=$(grep -o 'WALL [0-9.]*' "$OUT/dd-$n-$h.txt" | cut -d' ' -f2); walls="$walls $w"
        [ -n "$w" ] && sumr=$(echo "scale=1; $sumr + 8192 / $w" | bc)
    done
    # aggregate over the slowest client's dd wall (the dd's start within
    # ~1 s of each other); pw also contains ssh connect + FUSE_CHECK (~4 s).
    maxw=$(echo $walls | tr ' ' '\n' | sort -n | tail -1)
    agg=$(echo "scale=1; $n * 8192 / ${maxw:-$pw}" | bc)
    ceil=$([ "$n" = 1 ] && echo 16700 || echo 44000)   # MiB/s-ish: 16.7 GB/s link, 44 GB/s 4-host NVMe
    pct=$(echo "scale=2; 100 * $agg / $ceil" | bc)
    mm="$(echo $walls | tr ' ' '\n' | sort -n | sed -n '1p;$p' | tr '\n' '/' | sed 's,/$,,')"
    printf '%d\t%s\t%s\t%s\t%s\t%s\n' "$n" "$pw" "$([ $bad = 0 ] && echo $agg || echo INVALID)" "$mm" "$sumr" "$pct" >> "$OUT/table.tsv"
    say "$n client(s): slowest dd ${maxw:-?}s -> $agg MiB/s aggregate ($pct % of ceiling); per-client walls:$walls"
    rs=$(srv_report_split "$OUT/report-split-$n.txt"); say "$rs"; echo "$n clients: $rs" >> "$OUT/report-split-summary.txt"
    wait
done
for h in $ALL_CLIENTS; do ssh_ 20 "$h" "rm -f $MNT/measure/dd-$h.bin" >/dev/null 2>&1 & done; wait
{
    echo "8 GiB dd+fsync own-file, build=$BUILD, $(date -u +%F). Sep 18: 1=639 4=251 9=202 MiB/s."
    column -t -s $'\t' "$OUT/table.tsv"
    echo; echo "server REPORT timing (efsd report-split lines during each phase; the fsync tail is here):"; cat "$OUT/report-split-summary.txt"
    echo
    echo "agg = clients*8 GiB / slowest client dd wall (ssh_wall includes ~4 s ssh+check overhead); sum_rates = sum of per-client 8 GiB/wall. Perf: perf-ddwall-*.txt (PERF=1). Raw: dd-*-*.txt"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
