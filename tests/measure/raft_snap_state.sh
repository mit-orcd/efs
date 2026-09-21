#!/bin/bash
# raft_snap_state.sh — W11: the Raft log never compacts on 19810 because a
# group's KV export exceeds the one-command SNAP cap (EFS_WIRE_RAFT_MAX_CMD),
# so `snapshot skipped ... snap_oversized` latches at start-up, raft.log
# grows forever and a restarted follower replays the whole log. Chunked
# InstallSnapshot is NOT specified — this script only measures the symptom
# so the user can decide. Read-only, ~20 s.
#
#   bash tests/measure/raft_snap_state.sh
#
# Per server: raft.log bytes and growth rate over SAMPLE_SECS, KV dir size,
# applied index per group, the `snapshot skipped` lines, and the lag of each
# voter behind the group's max commit. Hand the user: log size vs the 4 MiB
# cap, bytes/day growth, and the replay cost (log bytes / follower catch-up
# rate seen in tests/roll_efsd.sh runs).
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
SAMPLE_SECS="${SAMPLE_SECS:-15}"
mkout raft-snap-state
preflight_or_die

printf 'host\traft_log_bytes\tlog_growth_B_per_s\tkv_bytes\tapplied0\tapplied2\tsnapshot_skipped_lines\n' > "$OUT/table.tsv"
declare -A s1
for h in $SERVERS; do s1[$h]=$(ssh_ 10 "$h" 'stat -c %s /data1/01/efs/mdraft/log/raft.log'); done
sleep "$SAMPLE_SECS"
for h in $SERVERS; do
    out=$(ssh_ 15 "$h" 'echo "$(stat -c %s /data1/01/efs/mdraft/log/raft.log) $(du -sb /data1/01/efs/mdraft/kv | cut -f1) $(python3 -c "import struct,sys;print(struct.unpack(\">Q\",open(\"/data1/01/efs/mdraft/applied.0\",\"rb\").read(8))[0])" 2>/dev/null || echo -) $(python3 -c "import struct;print(struct.unpack(\">Q\",open(\"/data1/01/efs/mdraft/applied.2\",\"rb\").read(8))[0])" 2>/dev/null || echo -) $(grep -c "snapshot skipped" /tmp/efs/efsd.log)"; grep "snapshot skipped" /tmp/efs/efsd.log | tail -2')
    l1=$(echo "$out" | head -1); echo "$out" | tail -n +2 | sed "s/^/$h: /" >> "$OUT/skipped-lines.txt"
    set -- $l1
    growth=$(( ($1 - ${s1[$h]:-$1}) / SAMPLE_SECS ))
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$h" "$1" "$growth" "$2" "$3" "$4" "$5" >> "$OUT/table.tsv"
done
say "raft-status from every voter (lag = group max commit - this node's commit)"
for h in $SERVERS; do
    ssh_ 10 "$h" "cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.$(( ${h#fcstor00} + 54 )):19810 2>/dev/null | grep 'hosted=1' | sed 's/^/$h /'" >> "$OUT/raft-status.txt"
done
cat "$OUT/raft-status.txt"
{
    echo "Raft log / snapshot state, build=$BUILD, $(date -u +%FT%TZ). SNAP cap = EFS_WIRE_RAFT_MAX_CMD (one command); a group whose KV export is bigger never compacts."
    column -t -s $'\t' "$OUT/table.tsv"
    echo; echo "snapshot-skipped lines (latched at start; the applied index in them is where compaction stopped being possible):"; cat "$OUT/skipped-lines.txt"
    echo; echo "voters:"; cat "$OUT/raft-status.txt"
    echo; echo "A follower restart replays the whole raft.log (bytes above) — see roll_efsd.sh catch-up times. Chunked InstallSnapshot is unspecified: ASK, do not design."
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
