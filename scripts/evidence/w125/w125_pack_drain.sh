#!/bin/bash
# w125_pack_drain.sh REL OUT [PORT] [FILES]
#
# Packer throughput on an idle private node9901 cluster (ports
# PORT+1..PORT+3, default 17681-17683; never orcd-mini or w100): write FILES
# small files (default 300,000; 16 workers, w125_p0_capture.py) with the
# packer held back (minimum age 1 h is not usable, so the files are written
# first at EFS_PACK_MIN_AGE_MS=600000 and the cluster is restarted at 0),
# then time how long the packer takes to pack all of them. OUT gets
# write.txt, drain.txt (seconds, files/s) and the packer log lines with
# their per-pass scan/read/segment timings.
set -u
REL=$1; OUT=$2; PORT=${3:-17680}; FILES=${4:-300000}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-drain-run
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
mkdir -p "$OUT"; "$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT >/dev/null 2>&1; rm -rf $RUN
EFS_PACK_MIN_AGE_MS=600000 "$HERE/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start1.txt" 2>&1 || { cat "$OUT/start1.txt"; exit 1; }
python3 "$HERE/w125_p0_capture.py" $RUN/mnt/w --workers 16 --max-files "$FILES" --out "$OUT/w.csv" > "$OUT/write.txt" 2>&1
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT >/dev/null 2>&1
t0=$(date +%s)
EFS_PACK_MIN_AGE_MS=0 "$HERE/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start2.txt" 2>&1 || { cat "$OUT/start2.txt"; exit 1; }
for t in $(seq 3600); do
  n=$(grep -h 'packed_total=' $RUN/logs/efsd-n*.log 2>/dev/null | sed 's/.*packed_total=\([0-9]*\).*/\1/' | sort -n | tail -1)
  [ "${n:-0}" -ge "$FILES" ] && break; sleep 1
done
t1=$(date +%s)
echo "files=$FILES packed=${n:-0} drain_s=$((t1 - t0)) rate=$(( ${n:-0} / (t1 - t0 > 0 ? t1 - t0 : 1) ))/s" > "$OUT/drain.txt"
grep -h 'packer:' $RUN/logs/efsd-n*.log > "$OUT/packer.log"
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT >/dev/null 2>&1; rm -rf $RUN
cat "$OUT/drain.txt"
