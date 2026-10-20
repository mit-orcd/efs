#!/bin/bash
# w125_budget_ab.sh BASE_REL PACK_REL OUT [PORT] [REPS] [FILES]
#
# Sustained-writer A/B for the packer's lag-controlled busy budget, on a
# private node9901 cluster (ports PORT+1..PORT+3, default 17681-17683;
# never orcd-mini or w100). Configurations, each on a fresh 3-daemon RDMA
# cluster (w125_p0_cluster.sh) with the default packer settings (minimum
# age 60 s):
#   B  base build, no packer
#   F  packer build, busy rate fixed at the floor (EFS_PACK_LAG_TARGET_MS=86400000)
#   A  packer build, lag-controlled busy rate (defaults: target 60 s, span 300 s)
# Workload: w125_p0_capture.py, 16 workers, FILES files (default 1,200,000,
# several minutes at ~3,000 files/s). Every 30 s during the write the
# script records node 1's per-file fragment and segment counts (backlog);
# after the write F and A wait until every file is packed (or 1,800 s).
# OUT/<cfg>/: capture CSV and rate, rates per minute
# (w125_budget_rates.py), backlog.txt, times.txt, packer log lines.
set -u
BASE=$1; PACK=$2; OUT=$3; PORT=${4:-17680}; REPS=${5:-1}; FILES=${6:-1200000}
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=/tmp/w125-budget-run
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
count() { echo "$(find $RUN/n1/data/exports -type f 2>/dev/null | wc -l) $(find $RUN/n1/data/segments -type f 2>/dev/null | wc -l)"; }
packed() { grep -h 'packed_total=' $RUN/logs/efsd-n*.log 2>/dev/null | sed 's/.*packed_total=//' | sort -n | tail -1; }
for rep in $(seq "$REPS"); do
for base_cfg in B F A; do
  cfg=$base_cfg; [ "$REPS" -gt 1 ] && cfg=$base_cfg-$rep
  unset EFS_PACK_LAG_TARGET_MS
  case $base_cfg in
    B) REL=$BASE ;;
    F) REL=$PACK; export EFS_PACK_LAG_TARGET_MS=86400000 ;;
    A) REL=$PACK ;;
  esac
  mkdir -p "$OUT/$cfg"; rm -rf "$RUN"
  "$HERE/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/$cfg/start.txt" 2>&1 || { cat "$OUT/$cfg/start.txt"; exit 1; }
  t0=$(date +%s)
  python3 "$HERE/w125_p0_capture.py" "$RUN/mnt/w" --workers 16 --max-files "$FILES" --out "$OUT/$cfg/w.csv" > "$OUT/$cfg/w.txt" 2>&1 &
  wp=$!
  while kill -0 $wp 2>/dev/null; do echo "$(( $(date +%s) - t0 )) frag_seg $(count) packed ${PACK:+$(packed)}" >> "$OUT/$cfg/backlog.txt"; sleep 30; done
  wait $wp
  t1=$(date +%s)
  echo "end-of-write $(( t1 - t0 )) frag_seg $(count) packed $(packed)" >> "$OUT/$cfg/backlog.txt"
  if [ $base_cfg != B ]; then
    for t in $(seq 1800); do [ "$(packed)" -ge "$FILES" ] 2>/dev/null && break; sleep 1; done
  fi
  t2=$(date +%s)
  echo "drained $(( t2 - t0 )) frag_seg $(count) packed $(packed)" >> "$OUT/$cfg/backlog.txt"
  echo "write_s=$(( t1 - t0 )) drain_s=$(( t2 - t1 ))" > "$OUT/$cfg/times.txt"
  python3 "$HERE/w125_budget_rates.py" "$OUT/$cfg/w.csv" > "$OUT/$cfg/rates.txt"
  grep -h 'packer:' $RUN/logs/efsd-n*.log > "$OUT/$cfg/packer.log" 2>/dev/null
  "$HERE/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1
done
done
rm -rf "$RUN"
echo done
