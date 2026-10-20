#!/bin/bash
# w125_mac_churn.sh OUT [ROUNDS] [FILES] [DELETE_PCT] [IDLE_S]
#
# Packing fragmentation over time on the small Mac VM cluster
# (w125_mac_cluster.sh: n1 efs2, n2/n3 efs3, TCP): fresh cluster, packer
# minimum age 5 s; ROUNDS (default 10) rounds of w125_churn.py on efs2,
# each writing FILES (default 1,500) files of 100 B - 16 KiB and deleting
# DELETE_PCT% (default 50) of all live files; 45 s after each round the
# packer's "segments" summaries (active segments, total/live/dead KiB,
# segments with holes, underfilled) and node 1's segment-file bytes are
# recorded. Compaction is scheduled maintenance: during the rounds the
# cluster counts as busy (EFS_PACK_BUSY_COMMITS_PER_S=10) so holes may
# accumulate; then an idle phase of IDLE_S (default 240) seconds, sampled
# every 30 s, shows the idle-triggered batches removing them
# (EFS_PACK_COMPACT_IDLE_S=20 for brevity; the daily window is moved out of
# the way). Ends with a byte-for-byte verify of every live file.
# Chaos environment: a VM reset (changed boot time on efs2/efs3) is logged
# as a CHAOS line; the daemons are restarted on their existing storage and
# the run resumes from the saved live set (a round cut by the reset is
# repeated; files it wrote stay as unlisted extra data).
set -u
OUT=$1; ROUNDS=${2:-10}; FILES=${3:-1500}; PCT=${4:-50}; IDLE=${5:-240}
HERE=$(cd "$(dirname "$0")" && pwd); D=/data1/w125/run
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=5000 QUOTA=${QUOTA:-4G} EFS_PACK_BUSY_COMMITS_PER_S=10 EFS_PACK_COMPACT_IDLE_S=20
# daily window: start 12 h from now (UTC minute of day), so only the idle trigger runs
export EFS_PACK_COMPACT_DAILY_START_MIN=$(( ( $(date -u +%H) * 60 + $(date -u +%M) + 720 ) % 1440 ))
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
for h in efs2 efs3; do ssh $h "rm -rf $D"; done
"$HERE/w125_mac_cluster.sh" start "$D" > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
scp -q "$HERE/w125_churn.py" efs2:/data1/w125/
ssh efs2 "rm -f /data1/w125/churn.json"
boots() { for h in efs2 efs3; do ssh -o ConnectTimeout=5 $h 'uptime -s' 2>/dev/null; done | tr '\n' ' '; }
B0=$(boots); r=1; tries=0
recover() {
  echo "CHAOS reset before/after round $r (boots $B0 -> $(boots)); restarting daemons on existing storage" | tee -a "$OUT/samples.txt" >> "$OUT/rounds.txt"
  for t in $(seq 60); do [ "$(boots | wc -w)" -ge 4 ] && break; sleep 5; done
  "$HERE/w125_mac_cluster.sh" start "$D" >> "$OUT/restart.txt" 2>&1
  B0=$(boots); tries=$((tries + 1))
}
while [ $r -le "$ROUNDS" ] && [ $tries -le 6 ]; do
  if ! ssh efs2 "python3 /data1/w125/w125_churn.py $D/mnt/c 1 $FILES $PCT /data1/w125/churn.json" >> "$OUT/rounds.txt" 2>&1; then
    if [ "$(boots)" != "$B0" ]; then recover; continue; fi
    echo "round $r failed (no reset)" >> "$OUT/rounds.txt"; break
  fi
  sleep 45
  if [ "$(boots)" != "$B0" ]; then recover; fi
  {
    echo "== after round $r: $(tail -n 1 "$OUT/rounds.txt")"
    for h in efs2 efs3; do ssh $h "grep -h 'packer: segments' $D/logs/efsd-n*.log | tail -n 2"; done | sort | tail -n 2 | sed 's/.*packer: //'
    ssh efs2 "echo n1 seg_files=\$(find $D/n1/data/segments -type f | wc -l) seg_kib=\$(du -sk $D/n1/data/segments | cut -f1) frag_files=\$(find $D/n1/data/exports -type f | wc -l)"
  } >> "$OUT/samples.txt"
  r=$((r + 1))
done
summary() {
  for h in efs2 efs3; do ssh $h "grep -h 'packer: segments' $D/logs/efsd-n*.log | tail -n 2"; done | sort | tail -n 2 | grep 'group=0' | sed 's/.*packer: //'
}
for t in $(seq 0 30 "$IDLE"); do
  [ "$(boots)" != "$B0" ] && { echo "CHAOS reset during the idle phase" >> "$OUT/samples.txt"; break; }
  echo "idle ${t}s $(summary)" >> "$OUT/samples.txt"; sleep 30
done
ssh efs2 "python3 /data1/w125/w125_churn.py $D/mnt/c verify 0 0 /data1/w125/churn.json" >> "$OUT/rounds.txt" 2>&1
for h in efs2 efs3; do ssh $h "grep -h 'packer: compact' $D/logs/efsd-n*.log"; done > "$OUT/compact.log"
"$HERE/w125_mac_cluster.sh" stop "$D" > /dev/null 2>&1
tail -n 1 "$OUT/rounds.txt"; grep -c . "$OUT/compact.log"
