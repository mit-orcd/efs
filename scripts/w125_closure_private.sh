#!/bin/bash
# w125_closure_private.sh REL OUT [PORT]
#
# W125 closure C2 (packed namespace) and C3 (size/read edges) on a private
# node9901 cluster (w125_p0_cluster.sh, ports PORT+1..PORT+3, default
# 17731-17733; never orcd-mini or w100), packer minimum age 3 s. After each
# test deletes its files, node 1 must reach no segment file within 120 s.
# Scripts are taken from this script's directory. Exit 1 on any FAIL.
set -u
ADDR=${W125_ADDR:-10.1.99.1} # node address; node9900 is 10.1.99.0
REL=$1; OUT=$2; PORT=${3:-17730}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-closure-run; SEED=$ADDR:$((PORT + 1))
mkdir -p "$OUT"; FAIL=0
export EFS_PACK_MIN_AGE_MS=3000 LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
"$REL/scripts/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
segs() { find $RUN/n1/data/segments -type f 2>/dev/null | wc -l; }
for t in namespace edges; do
  python3 "$HERE/w125_packed_$t.py" $RUN/mnt "$REL/efs-mgmt" $SEED > "$OUT/$t.txt" 2>&1 || FAIL=1
  for i in $(seq 40); do [ "$(segs)" -eq 0 ] && break; sleep 3; done
  echo "$t: $(tail -n 1 "$OUT/$t.txt"); segments left on n1: $(segs)" | tee -a "$OUT/summary.txt"
  [ "$(segs)" -eq 0 ] || { echo "FAIL $t left segments" | tee -a "$OUT/summary.txt"; FAIL=1; }
done
grep -a "efs_rc=" $RUN/logs/fuse.log | grep -v report-split > "$OUT/client-errors.txt"
echo "client error lines: $(grep -c . "$OUT/client-errors.txt")" | tee -a "$OUT/summary.txt"
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
[ -s "$OUT/client-errors.txt" ] && FAIL=1
echo "RESULT $([ $FAIL = 0 ] && echo PASS || echo FAIL)" | tee -a "$OUT/summary.txt"
exit $FAIL
