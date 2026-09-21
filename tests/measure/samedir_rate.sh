#!/bin/bash
# samedir_rate.sh — W6 residual 3: what is the ~200 ops/s same-directory
# ceiling made of?
#
# Runs tests/stress/same_parent_storm.sh at increasing contention with the
# clients remounted under EFS_RPC_PROF=1 (same binary; the client prints a
# cumulative RPC-PROF line to fuse.log every 2 s: calls, BUSY-backoff count
# and time, per-op recv time). Clients are remounted before each level so
# the counters are per level. After the biggest level it dumps the raft-log
# command histogram of both group leaders and, with PERF=1, a 20 s cpu-clock
# profile of both leaders' efsd taken DURING the storm.
#
# Usage (from node9901 via efs-bg.sh; ~6 min default):
#   bash tests/measure/samedir_rate.sh
#   LEVELS="1x1 1x9 4x9" ROUNDS=100 PERF=1 bash tests/measure/samedir_rate.sh
# LEVELS are procs-per-host x hosts (hosts taken from the front of the
# 9-client list). ROUNDS = mkdir/create/rmdir/unlink rounds per proc.
#
# Reading the SUMMARY: ms/op is wall / (4*ROUNDS) per proc. If busy_us is a
# large share of the wall, the 50 ms x 2^n BUSY backoff is the ceiling
# (same-parent CREATE vs RMDIR on the dseq witness are mutually BUSY by
# design). If busy_us is small and recv_us per call grows with contention,
# the server side (apply pump / Raft commit queue) is; the perf profile and
# the histogram say which. Hand the table + both to the user; do not change
# the backoff or the protocol on your own.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
LEVELS="${LEVELS:-1x1 1x9 4x9}"
ROUNDS="${ROUNDS:-100}"
PERF="${PERF:-0}"
mkout samedir-rate
preflight_or_die

printf 'level\tprocs\tops\twall_s\tops_per_s\tms_per_op_proc\tcalls\tbusy_n\tbusy_s\trecv_s\tcreate_n\tcreate_avg_us\tunlink_n\tunlink_avg_us\n' > "$OUT/table.tsv"
for lvl in $LEVELS; do
    procs=${lvl%x*}; nh=${lvl#*x}
    hosts=$(echo $ALL_CLIENTS | tr ' ' '\n' | head -"$nh" | tr '\n' ' ')
    hosts_ib=$(for h in $hosts; do printf '%s.ib ' "$h"; done)
    say "== level $lvl: remount $nh client(s) with EFS_RPC_PROF=1"
    remount_clients "$hosts" "EFS_RPC_PROF=1" > "$OUT/remount-$lvl.txt" || { say "remount failed"; cat "$OUT/remount-$lvl.txt"; exit 1; }
    if [ "$PERF" = 1 ] && [ "$lvl" = "${LEVELS##* }" ]; then
        l0=$(leader_host 0); l2=$(leader_host 2)
        say "perf on leaders g0=$l0 g2=$l2 during the storm"
        ( sleep 8; perf_daemon "$l0" efsd 20 "samedir-g0" ) &
        ( sleep 8; perf_daemon "$l2" efsd 20 "samedir-g2" ) &
    fi
    t0=$(date +%s.%N)
    PROCS=$procs ROUNDS=$ROUNDS HOSTS="$hosts_ib" OUTDIR="$OUT/storm-$lvl" \
        bash "$REPO/tests/stress/same_parent_storm.sh" > "$OUT/storm-$lvl.log" 2>&1
    rc=$?
    wall=$(echo "$(date +%s.%N) - $t0" | bc)
    wait
    grep -E 'PASS|FAIL' "$OUT/storm-$lvl.log" | tail -3
    [ $rc = 0 ] || say "WARNING: storm at $lvl did not PASS (see storm-$lvl.log) — a correctness regression, report it first"
    # per-proc wall from the DONE lines is the honest number (excludes ssh setup)
    pwall=$(grep -h '^DONE' "$OUT/storm-$lvl"/log-*.txt | sed 's/.*secs=//' | sort -n | tail -1)
    nproc=$(( procs * nh )); ops=$(( nproc * ROUNDS * 4 ))
    calls=0; busy_n=0; busy_us=0; recv_us=0; cn=0; cus=0; un=0; uus=0
    for h in $hosts; do
        line=$(rpc_prof_last "$h"); echo "$h $line" >> "$OUT/rpcprof-$lvl.txt"
        [ -n "$line" ] || { say "WARNING: no RPC-PROF line on $h (was it remounted with EFS_RPC_PROF=1?)"; continue; }
        calls=$(( calls + $(n0 "$(rpc_field "$line" calls)") ))
        busy_n=$(( busy_n + $(n0 "$(rpc_field "$line" busy_n)") ))
        busy_us=$(( busy_us + $(n0 "$(rpc_field "$line" busy_us)") ))
        recv_us=$(( recv_us + $(n0 "$(rpc_field "$line" recv_us)") ))
        cn=$(( cn + $(n0 "$(rpc_field_n "$line" create)") )); cus=$(( cus + $(n0 "$(rpc_field_us "$line" create)") ))
        un=$(( un + $(n0 "$(rpc_field_n "$line" unlink)") )); uus=$(( uus + $(n0 "$(rpc_field_us "$line" unlink)") ))
    done
    ops_s=$(echo "scale=1; $ops / $pwall" | bc)
    ms_op=$(echo "scale=2; $pwall * 1000 / ($ROUNDS * 4)" | bc)
    cavg=$([ "$cn" -gt 0 ] && echo $(( cus / cn )) || echo 0)
    uavg=$([ "$un" -gt 0 ] && echo $(( uus / un )) || echo 0)
    printf '%s\t%d\t%d\t%s\t%s\t%s\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\n' "$lvl" "$nproc" "$ops" "$pwall" "$ops_s" "$ms_op" \
        "$calls" "$busy_n" "$(echo "scale=1; $busy_us/1000000" | bc)" "$(echo "scale=1; $recv_us/1000000" | bc)" "$cn" "$cavg" "$un" "$uavg" >> "$OUT/table.tsv"
    say "level $lvl: $ops ops in ${pwall}s = $ops_s ops/s, $ms_op ms/op/proc, busy_n=$busy_n busy=$(echo "scale=1; $busy_us/1000000" | bc)s recv=$(echo "scale=1; $recv_us/1000000" | bc)s"
done

say "raft-log histograms of both leaders (last 4 MiB, dominated by the last level)"
for g in 0 2; do
    l=$(leader_host $g); echo "== group $g leader $l" >> "$OUT/raft-hist.txt"
    raft_hist "$l" >> "$OUT/raft-hist.txt"
done

say "restore clients (plain remount, no RPC_PROF)"
remount_clients "$ALL_CLIENTS" "" > "$OUT/remount-restore.txt" || say "WARNING: restore remount had failures: $(grep -v MOUNT_OK "$OUT/remount-restore.txt")"

{
    echo "same-directory op rate, build=$BUILD, $(date -u +%F), ROUNDS=$ROUNDS"
    echo "ops = 4 per round (mkdir, create, rmdir, unlink) in ONE parent; ms/op = per proc; busy_s/recv_s summed over clients"
    column -t -s $'\t' "$OUT/table.tsv"
    echo
    echo "busy_s / (procs * wall) = share of each proc's time in BUSY backoff; recv_s likewise for server+wire wait."
    echo "Histograms: raft-hist.txt. Perf: perf-samedir-g*.txt (PERF=1). Raw: storm-*/ rpcprof-*.txt"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
