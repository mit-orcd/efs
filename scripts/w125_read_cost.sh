#!/bin/bash
# w125_read_cost.sh REL OUT [PORT] [FILES]
#
# W125 closure C8: read cost of packed vs unpacked small files on a private
# node9901 cluster (w125_p0_cluster.sh, ports PORT+1..PORT+3, default
# 17741-17743; never orcd-mini or w100). FILES (default 200,000) files of
# 1 B - 16 KiB are written with packing held back (minimum age 1 h); the
# client is remounted (cold client cache) and tiny_files_md5.py (16 workers)
# hashes everything: the unpacked crawl. The cluster is then restarted on
# the same storage with minimum age 0; once node 1 has no per-file small
# fragments left, the client is remounted and the crawl repeated: the packed
# crawl. Both manifests must agree. Prints files/s and MiB/s of both crawls.
set -u
REL=$1; OUT=$2; PORT=${3:-17740}; FILES=${4:-200000}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-rcost-run
mkdir -p "$OUT"; export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
C="$REL/scripts/w125_p0_cluster.sh"
$C stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
EFS_PACK_MIN_AGE_MS=3600000 $C start "$REL" $RUN $PORT > "$OUT/start1.txt" 2>&1 || { cat "$OUT/start1.txt"; exit 1; }
python3 "$HERE/tiny_files.py" $RUN/mnt/t --max-files "$FILES" --max-file-size 16384 --max-depth 2 --max-files-per-folder 500 --workers 16 > "$OUT/write.txt" 2>&1
crawl() { # label
  "$REL/scripts/client.sh" stop $RUN/mnt > /dev/null 2>&1
  EFS_FUSE_LOG=$RUN/logs/fuse-$1.log "$REL/scripts/client.sh" 10.1.99.1:$((PORT + 1)) $RUN/mnt > /dev/null 2>&1
  local t0 t1; t0=$(date +%s.%N)
  python3 "$HERE/tiny_files_md5.py" $RUN/mnt/t --workers 16 --output "$OUT/$1.jsonl" > "$OUT/$1.out" 2>&1
  t1=$(date +%s.%N)
  python3 - "$OUT/$1.jsonl" "$(echo "$t1 - $t0" | bc)" "$1" <<'PY' | tee -a "$OUT/summary.txt"
import json, sys
n = b = e = 0
for line in open(sys.argv[1]):
    r = json.loads(line)
    if r.get('type') != 'file': continue
    if r.get('status') == 'ok': n += 1; b += r.get('bytes', 0)
    else: e += 1
t = float(sys.argv[2])
print("%s crawl: files=%d errors=%d seconds=%.1f files_per_s=%.0f MiB_per_s=%.1f" % (sys.argv[3], n, e, t, n / t, b / t / 2**20))
PY
}
crawl unpacked
$C stop "$REL" $RUN $PORT > /dev/null 2>&1
EFS_PACK_MIN_AGE_MS=0 $C start "$REL" $RUN $PORT > "$OUT/start2.txt" 2>&1
for t in $(seq 240); do [ "$(find $RUN/n1/data/exports -type f | wc -l)" -lt 100 ] && break; sleep 5; done
echo "packed: per-file fragments left on n1 $(find $RUN/n1/data/exports -type f | wc -l), segments $(find $RUN/n1/data/segments -type f | wc -l)" | tee -a "$OUT/summary.txt"
sleep 30
crawl packed
python3 "$HERE/w125_md5_compare.py" "$OUT/unpacked.jsonl" "$OUT/packed.jsonl" | tee -a "$OUT/summary.txt"
$C stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
