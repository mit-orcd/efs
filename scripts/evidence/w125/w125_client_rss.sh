#!/bin/bash
# w125_client_rss.sh REL OUT [PORT] [ROUNDS]
#
# Client memory trajectory on a private node9901 cluster (w125_p0_cluster.sh,
# ports PORT+1..PORT+3, default 17721-17723; never orcd-mini or w100):
# ROUNDS (default 4) rounds of 180 s of w125_concurrent.py (8 writers,
# 8 verifying readers, 4 deleters; each round in a new directory, the
# previous one removed) with 60 s idle after each round. The efs-fuse RSS,
# thread and fd counts are sampled every 10 s (rss.tsv). A bounded client
# plateaus; a leak keeps rising round over round. Prints the RSS at the end
# of each round and of each idle period.
set -u
REL=$1; OUT=$2; PORT=${3:-17720}; ROUNDS=${4:-4}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-rss-run
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=2000 LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
"$HERE/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
P=$(pgrep -f "efs-fuse .* $RUN/mnt" | head -1)
( t0=$(date +%s); while kill -0 $P 2>/dev/null; do
    printf '%s\t%s\t%s\t%s\n' $(( $(date +%s) - t0 )) "$(awk '/VmRSS/ {print $2}' /proc/$P/status)" \
      "$(ls /proc/$P/task | wc -l)" "$(ls /proc/$P/fd | wc -l)" >> "$OUT/rss.tsv"; sleep 10; done ) &
rss() { awk '/VmRSS/ {print $2}' /proc/$P/status; }
for r in $(seq "$ROUNDS"); do
  python3 "$HERE/w125_concurrent.py" $RUN/mnt/r$r 180 --writers 8 --readers 8 --deleters 4 > "$OUT/conc$r.txt" 2>&1
  echo "round $r: $(grep '^final' "$OUT/conc$r.txt") rss_kib=$(rss)"
  rm -rf $RUN/mnt/r$r $RUN/mnt/r$r.live $RUN/mnt/r$r.missing
  sleep 60; echo "idle $r: rss_kib=$(rss)"
done | tee "$OUT/summary.txt"
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
