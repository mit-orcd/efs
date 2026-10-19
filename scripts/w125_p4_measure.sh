#!/bin/bash
# w125_p4_measure.sh BASE_REL PACK_REL OUT [PORT] [REPS]
#
# W125 P4 early measurement on a private node9901 cluster (never orcd-mini or
# w100). Three configurations, each on a fresh 3-daemon cluster started by
# w125_p0_cluster.sh:
#   B   base build (no packer)
#   PI  packer build, packer idle (EFS_PACK_MIN_AGE_MS=3600000): the cost of
#       the candidate index alone
#   PA  packer build, packer active (EFS_PACK_MIN_AGE_MS=2000): packing runs
#       concurrently with the foreground workload
# Workload (scripts/w125_p0_capture.py, the replicated tiny_files.py):
#   r1  16 workers, 50,000 files; r2 1 worker, 10,000 files.
# After the workload, PA waits until every candidate is packed (or 600 s).
# Per configuration OUT/<cfg>/ gets the capture CSVs and rates, store
# snapshots (file/dir counts and bytes per node), packer log lines and
# raft status. With REPS > 1 the three configurations run round-robin REPS
# times (OUT/<cfg>-<rep>/), so drift affects all of them alike. Raw output
# stays outside Git.
set -u
BASE=$1; PACK=$2; OUT=$3; PORT=${4:-17680}; REPS=${5:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=/tmp/w125-p4-run
snap() { # cfg tag
  for i in 1 2 3; do
    d=$RUN/n$i/data
    printf 'n%s %s frag_files=%s frag_bytes=%s seg_files=%s seg_bytes=%s dirs=%s\n' "$i" "$2" \
      "$(find $d/exports -type f 2>/dev/null | wc -l)" "$(du -sb $d/exports 2>/dev/null | cut -f1)" \
      "$(find $d/segments -type f 2>/dev/null | wc -l)" "$(du -sb $d/segments 2>/dev/null | cut -f1)" \
      "$(find $d -type d 2>/dev/null | wc -l)"
  done >> "$OUT/$1/snap.txt"
}
for rep in $(seq "$REPS"); do
for base_cfg in B PI PA; do
  cfg=$base_cfg; [ "$REPS" -gt 1 ] && cfg=$base_cfg-$rep
  case $base_cfg in
    B) REL=$BASE; AGE=0 ;;
    PI) REL=$PACK; AGE=3600000 ;;
    PA) REL=$PACK; AGE=2000 ;;
  esac
  mkdir -p "$OUT/$cfg"; rm -rf "$RUN"
  EFS_PACK_MIN_AGE_MS=$AGE "$HERE/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/$cfg/start.txt" 2>&1 || { cat "$OUT/$cfg/start.txt"; exit 1; }
  snap $cfg start
  t0=$(date +%s.%N)
  python3 "$HERE/w125_p0_capture.py" "$RUN/mnt/r1" --workers 16 --max-files 50000 --out "$OUT/$cfg/r1.csv" > "$OUT/$cfg/r1.txt" 2>&1
  python3 "$HERE/w125_p0_capture.py" "$RUN/mnt/r2" --workers 1 --max-files 10000 --out "$OUT/$cfg/r2.csv" > "$OUT/$cfg/r2.txt" 2>&1
  t1=$(date +%s.%N)
  snap $cfg after-write
  if [ $base_cfg = PA ]; then
    for t in $(seq 600); do
      n=$(grep -h 'packed_total=' $RUN/logs/efsd-n*.log | sed 's/.*packed_total=//' | sort -n | tail -1)
      [ "${n:-0}" -ge 60000 ] && break; sleep 1
    done
  fi
  t2=$(date +%s.%N)
  sleep 15 # let the reclaim journals drain
  snap $cfg settled
  echo "write_s=$(echo "$t1 - $t0" | bc) pack_wait_s=$(echo "$t2 - $t1" | bc)" > "$OUT/$cfg/times.txt"
  grep -h 'packer:' $RUN/logs/efsd-n*.log > "$OUT/$cfg/packer.log" 2>/dev/null
  "$REL/efs-mgmt" raft-status 10.1.99.1:$((PORT+1)) > "$OUT/$cfg/raft.txt" 2>&1
  "$HERE/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1
done
done
rm -rf "$RUN"
echo done
