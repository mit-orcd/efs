#!/bin/bash
# w8_posix9_probe.sh — the 9-host POSIX suite (W8's gate) with live
# snapshots taken WHILE it runs, so a cliff of "timeout after 15s" rows
# (Sep 21: 116 per host, every test after the alphabet's first quarter)
# comes with evidence instead of a post-mortem of quiet logs.
#
# Runs run_tests.sh posix --parallel on the 9 clients (detached), waits
# PROBE_AT seconds, then on each probe client: gdb backtrace of efs-fuse
# (function histogram + every thread in an RPC recv), ss of its 19810
# conns; on every server: gdb backtrace of efsd (histogram of non-idle
# stacks, read_mu holders), one RPC-PROF line per client; a second probe
# 30 s later. Then waits for the suite and prints per-host pass/fail/none
# counts plus a per-test timing view (which alphabet position timed out
# first on each host).
#
# Usage (from node9901 via efs-bg.sh; ~5 min):
#   bash tests/measure/w8_posix9_probe.sh
#   PROBE_AT=30 PROBE_CLIENTS="fcstor008 fcstor012" bash tests/measure/w8_posix9_probe.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
PROBE_AT=${PROBE_AT:-45}
PROBE_CLIENTS=${PROBE_CLIENTS:-fcstor008 fcstor013}
mkout w8-posix9-probe
preflight_or_die

# RPC-PROF on the clients: per-op recv time is the number that says
# whether the wall is the server. Same binary, remount only.
say "== remount 9 clients with EFS_RPC_PROF=1"
remount_clients "$ALL_CLIENTS" "EFS_RPC_PROF=1" > "$OUT/remount.txt" 2>&1
grep -cE "MOUNT_OK|FUSE_OK" "$OUT/remount.txt" | sed 's/^/mounted: /'

say "== start the 9-host suite (detached)"
( cd "$REPO" && EFS_TRANSPORT=tcp bash tests/run_tests.sh posix --parallel \
    fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib \
    > "$OUT/suite.log" 2>&1 ) &
SUITE_PID=$!
T0=$(date +%s)

probe() {
    local tag=$1 h
    say "== probe $tag at +$(( $(date +%s) - T0 ))s"
    for h in $PROBE_CLIENTS; do
        ssh_ 40 "$h" 'pid=$(pgrep -x efs-fuse | head -1); [ -n "$pid" ] || { echo NO_FUSE; exit 0; }
            timeout 30 gdb -p $pid -batch -ex "set pagination off" -ex "thread apply all bt 10" 2>/dev/null > /tmp/fuse-bt.txt
            echo "threads=$(grep -c ^Thread /tmp/fuse-bt.txt)"
            echo "-- threads in an RPC (rpc_send_recv_*), by top efs frame:"
            awk "/^Thread/{t=\$0; s=\"\"} /^#/{gsub(/^#[0-9]+ +(0x[0-9a-f]+ in )?/,\"\"); sub(/ \\(.*/,\"\"); s=s\" > \"\$1} /^\$/{if(s ~ /rpc_send_recv/) print s}" /tmp/fuse-bt.txt | sort | uniq -c | sort -rn | head -12
            echo "-- fuse workers not in fuse_do_work read (busy):"
            awk "/^Thread/{t=\$0; s=\"\"} /^#/{gsub(/^#[0-9]+ +(0x[0-9a-f]+ in )?/,\"\"); sub(/ \\(.*/,\"\"); s=s\" > \"\$1} /^\$/{if(s ~ /fuse_do_work|fuse_session_loop/ && s !~ /fuse_session_receive_buf_int/) print s}" /tmp/fuse-bt.txt | cut -c1-300 | sort | uniq -c | sort -rn | head -12
            echo "-- conns to 19810 (Recv-Q Send-Q peer):"; ss -tn state established "( dport = :19810 )" | awk "NR>1{print \$1, \$2, \$4}" | sort | uniq -c | sort -rn | head
            grep RPC-PROF /tmp/efs/fuse.log | tail -1 | cut -c1-600' > "$OUT/probe-$tag-$h.txt" 2>&1
        echo "  $h: $(grep -m1 threads= "$OUT/probe-$tag-$h.txt") rpc-threads=$(awk '/threads in an RPC/{f=1;next} /fuse workers/{f=0} f{s+=$1} END{print s+0}' "$OUT/probe-$tag-$h.txt")"
    done
    for h in $SERVERS; do
        ssh_ 40 "$h" 'pid=$(pgrep -x efsd | head -1); [ -n "$pid" ] || { echo NO_EFSD; exit 0; }
            timeout 30 gdb -p $pid -batch -ex "set pagination off" -ex "thread apply all bt 14" 2>/dev/null > /tmp/efsd-bt.txt
            echo "threads=$(grep -c ^Thread /tmp/efsd-bt.txt)"
            echo "-- conn threads (server_handle_conn) not idle in recv, by stack:"
            awk "/^Thread/{s=\"\"} /^#/{gsub(/^#[0-9]+ +(0x[0-9a-f]+ in )?/,\"\"); sub(/ \\(.*/,\"\"); s=s\" > \"\$1} /^\$/{if(s ~ /server_handle_conn/ && s !~ /^ > recv > efs_recv_all > server_handle_conn/) print s}" /tmp/efsd-bt.txt | cut -c1-400 | sort | uniq -c | sort -rn | head -14
            echo "-- waiting on read_mu (lll_lock under a server_raft_host_ frame):"
            awk "/^Thread/{s=\"\"} /^#/{gsub(/^#[0-9]+ +(0x[0-9a-f]+ in )?/,\"\"); sub(/ \\(.*/,\"\"); s=s\" > \"\$1} /^\$/{if(s ~ /lll_lock|pthread_mutex_lock/ && s ~ /server_raft_host_/) print s}" /tmp/efsd-bt.txt | cut -c1-300 | sort | uniq -c | sort -rn | head -6
            top -b -H -n1 -p $pid | awk "NR>7 && \$9>=5.0 {print \"  cpu\", \$9, \$12}" | head -5' > "$OUT/probe-$tag-$h.txt" 2>&1
        echo "  $h: $(grep -m1 threads= "$OUT/probe-$tag-$h.txt") busy-conn-threads=$(awk '/conn threads/{f=1;next} /waiting on read_mu/{f=0} f{s+=$1} END{print s+0}' "$OUT/probe-$tag-$h.txt") read_mu-waiters=$(awk '/waiting on read_mu/{f=1;next} /^  cpu/{f=0} f{s+=$1} END{print s+0}' "$OUT/probe-$tag-$h.txt")"
    done
}

sleep "$PROBE_AT"
probe 1
sleep 30
probe 2
say "== waiting for the suite"
wait $SUITE_PID
RID=$(grep -o 'results/posix/[0-9-]*' "$OUT/suite.log" | tail -1)
say "suite results: $RID"

{
    echo "9-host posix suite on $BUILD, probes at +${PROBE_AT}s and +$((PROBE_AT+30))s; suite dir $RID"
    printf '%-9s %5s %5s %5s %5s %7s %s\n' host rows pass fail none dur first_timeout
    for h in $ALL_CLIENTS; do
        f="$REPO/$RID/efs-$h.tsv"
        [ -f "$f" ] || { echo "$h NO_TSV"; continue; }
        printf '%-9s %5s %5s %5s %5s %7s %s\n' "$h" \
            "$(awk -F'\t' 'NF>=2 && $1!~/^#/' "$f" | wc -l)" \
            "$(awk -F'\t' '$2=="PASS"' "$f" | wc -l)" \
            "$(awk -F'\t' '$2=="FAIL"' "$f" | wc -l)" \
            "$(awk -F'\t' '$2=="NOTRUN"' "$f" | wc -l)" \
            "$(grep -o 'dur=[0-9.]*' "$f" | cut -d= -f2)" \
            "$(awk -F'\t' '$2=="FAIL" && $3~/timeout/{print $1; exit}' "$f")"
    done
    echo
    echo "RPC-PROF (last line per client): lookup/getattr/create/unlink avg us"
    for h in $ALL_CLIENTS; do
        l=$(rpc_prof_last "$h")
        printf '%-9s calls=%s recv_s=%s lookup=%s/%sus getattr=%s/%sus create=%s/%sus unlink=%s/%sus\n' "$h" \
            "$(rpc_field "$l" calls)" "$(( $(n0 "$(rpc_field "$l" recv_us)") / 1000000 ))" \
            "$(rpc_field_n "$l" lookup)" "$(( $(n0 "$(rpc_field_us "$l" lookup)") / ( $(n0 "$(rpc_field_n "$l" lookup)") + 1 ) ))" \
            "$(rpc_field_n "$l" getattr)" "$(( $(n0 "$(rpc_field_us "$l" getattr)") / ( $(n0 "$(rpc_field_n "$l" getattr)") + 1 ) ))" \
            "$(rpc_field_n "$l" create)" "$(( $(n0 "$(rpc_field_us "$l" create)") / ( $(n0 "$(rpc_field_n "$l" create)") + 1 ) ))" \
            "$(rpc_field_n "$l" unlink)" "$(( $(n0 "$(rpc_field_us "$l" unlink)") / ( $(n0 "$(rpc_field_n "$l" unlink)") + 1 ) ))"
    done
    echo
    echo "Probe summaries: probe-*-fcstor0NN.txt (client: RPC threads + conns; server: busy conn threads, read_mu waiters)"
} | tee "$OUT/SUMMARY.txt"

say "== restore clients (plain remount)"
remount_clients "$ALL_CLIENTS" "" > "$OUT/remount-restore.txt" 2>&1
echo DONE
