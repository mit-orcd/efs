#!/bin/bash
# Run ON node9901 (detached): sample orcd-mini reclaim state every INTERVAL s.
# Usage: OUT=/data1/efs/prof/w124/observe.tsv INTERVAL=10 orcd_mini_w124_observe.sh
# One TSV row per node per sample: time, node, data_usage_bytes,
# reclaim_pending_bytes, reclaim accepted/deleted/retired/busy, journal
# records/segments, reclaim_state, phys_avail_bytes, phys_refused, both
# groups' reap_seen and capped (leader only), removed_fragments, /data1
# available bytes, efsd CPU seconds. Stop with kill; the file stays.
set -u
OUT=${OUT:-/data1/efs/prof/w124/observe.tsv}
INTERVAL=${INTERVAL:-10}
MGMT=${MGMT:-/data1/efs/src/efs-mgmt}
mkdir -p "$(dirname "$OUT")"
[ -s "$OUT" ] || printf 'time\tnode\tdata_usage\tpending\taccepted\tdeleted\tretired\tbusy\tjournal_records\tjournal_segments\tstate\tphys_avail\tphys_refused\treap0\tcapped0\treap2\tcapped2\tremoved_fragments\tfs_avail\tcpu_s\n' > "$OUT"
while :; do
  now=$(date -u +%FT%TZ)
  avail=$(df -B1 --output=avail /data1 | tail -1 | tr -d ' ')
  for i in 1 2 3; do
    port=$((17431 + i))
    s=$($MGMT gc-status 10.1.99.1:$port 2>/dev/null)
    get() { printf '%s\n' "$s" | grep -o "\b$1=[^ ]*" | head -1 | cut -d= -f2; }
    g0=$(printf '%s\n' "$s" | grep '^group=0 ')
    g2=$(printf '%s\n' "$s" | grep '^group=2 ')
    gv() { printf '%s\n' "$1" | grep -o "\b$2=[^ ]*" | cut -d= -f2; }
    pid=$(pgrep -f "efsd --cluster .*--port $port( |$)" | head -1)
    cpu=""
    [ -n "$pid" ] && cpu=$(awk '{print ($14+$15)/100}' /proc/$pid/stat 2>/dev/null)
    printf '%s\tn%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$now" "$i" "$(get data_usage_bytes)" "$(get reclaim_pending_bytes)" "$(get reclaim_accepted)" \
      "$(get reclaim_deleted)" "$(get reclaim_retired)" "$(get reclaim_busy)" "$(get journal_records)" \
      "$(get journal_segments)" "$(get reclaim_state)" "$(get phys_avail_bytes)" "$(get phys_refused)" \
      "$(gv "$g0" reap_seen)" "$(gv "$g0" capped)" "$(gv "$g2" reap_seen)" "$(gv "$g2" capped)" \
      "$(get removed_fragments)" "$avail" "$cpu" >> "$OUT"
  done
  sleep "$INTERVAL"
done
