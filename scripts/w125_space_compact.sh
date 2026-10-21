#!/bin/bash
# w125_space_compact.sh OUT
#
# W125 closure C4 (space accounting) and C5 (unattended compaction) on
# orcd-mini, run ON node9901 against /data1/efs/mnt (seed 10.1.99.1:17432,
# storage roots /data1/efs/n1-3). No efs-mgmt pack-compact is issued here.
#   C4: at each step compare, per node, efs-mgmt status "used" with the
#       bytes of data/exports + data/segments on disk (du -sb), and df on the
#       mount: empty, after writing 60,000 files (1 B - 16 KiB, 16 workers,
#       tiny_files.py-style deterministic content), after packing, after
#       deleting 50% at random, after compaction, after deleting all.
#   C5: three churn rounds (write 30,000 more, delete 50% of everything) with
#       the cluster busy, then 15 min idle and read-only (an md5 pass every
#       5 min): the fragmentation summary (efs-mgmt pack-compact SEED 0, which
#       only reports) must reach 0 holed segments by the idle-triggered
#       batches alone; every survivor must verify.
# One line per step in OUT/log.txt; exit 1 on a failed check.
set -u
OUT=$1; M=/data1/efs/mnt/sc-$(date -u +%H%M); MG=/data1/efs/src/efs-mgmt; SEED=10.1.99.1:17432
HERE=$(cd "$(dirname "$0")" && pwd); mkdir -p "$OUT" $M; FAIL=0
log() { echo "$(date -u +%T) $*" | tee -a "$OUT/log.txt"; }
summary() { $MG pack-compact $SEED 0 2>&1 | grep -o 'segments=.*' | tr '\n' ' '; }
space() { # step label
  local s=""
  for i in 1 2 3; do
    u=$($MG status $SEED 2>/dev/null | grep "node $i:" | grep -o 'used=[^q]*' | tr -d ' ')
    d=$(du -sb /data1/efs/n$i/data/exports /data1/efs/n$i/data/segments 2>/dev/null | awk '{s+=$1} END {print s+0}')
    s="$s n$i:$u disk=$((d / 1048576))MiB"
  done
  log "C4 $1:$s df_used=$(df -B1M --output=used /data1/efs/mnt | tail -1 | tr -d ' ')MiB | $(summary)"
}
write() { python3 "$HERE/w125_churn.py" $M "$1" "$2" 0 $OUT/state.json > /dev/null; }  # rounds files pct
space empty
write 6 10000; space "wrote 60000"
for t in $(seq 60); do sleep 10; [ "$($MG pack-compact $SEED 0 2>&1 | grep -c leads)" -ge 1 ] && \
  [ "$(find /data1/efs/n1/data/exports -type f | wc -l)" -lt 200 ] && break; done
space packed
python3 - $M $OUT/state.json 50 <<'PY'
import json, os, random, sys
root, st, pct = sys.argv[1], sys.argv[2], int(sys.argv[3]); live = json.load(open(st)); r = random.Random(5)
gone = set(r.sample(range(len(live)), len(live) * pct // 100))
for i in gone: os.unlink(os.path.join(root, live[i]))
json.dump([p for i, p in enumerate(live) if i not in gone], open(st, "w"))
PY
sleep 30; space "deleted 50%"
log "C5 churn: 3 rounds of 30,000 writes and 50% deletes (busy)"
for r in 1 2 3; do python3 "$HERE/w125_churn.py" $M 1 30000 50 $OUT/state.json >> "$OUT/churn.txt"; done
log "C5 idle start: $(summary)"
for t in $(seq 30); do
  sleep 30
  [ $((t % 10)) -eq 0 ] && python3 "$HERE/w125_churn.py" $M verify 0 0 $OUT/state.json >> "$OUT/verify.txt" 2>&1
  log "C5 idle $((t * 30))s: $(summary)"
done
h=$(summary | grep -o 'holed=[0-9]*' | cut -d= -f2 | paste -sd+ | bc)
[ "${h:-1}" -eq 0 ] || { log "FAIL C5 holes left after 15 min idle: $h"; FAIL=1; }
python3 "$HERE/w125_churn.py" $M verify 0 0 $OUT/state.json >> "$OUT/verify.txt" 2>&1 || { log "FAIL C5 verify"; FAIL=1; }
log "C5 verify: $(tail -n 1 "$OUT/verify.txt")"
space compacted
rm -rf $M; sleep 60; space "deleted all"
log "RESULT $([ $FAIL = 0 ] && echo PASS || echo FAIL)"
exit $FAIL
