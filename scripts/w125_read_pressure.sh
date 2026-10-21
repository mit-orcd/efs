#!/bin/bash
# w125_read_pressure.sh REL OUT [PORT] [SECONDS]
#
# Reads under client write-buffer pressure, on a private node9901 cluster
# (w125_p0_cluster.sh, ports PORT+1..PORT+3, default 17701-17703; never
# orcd-mini or w100): the client data-cache budget is shrunk to its minimum
# (EFS_DCACHE_HARD_BYTES=EFS_DCACHE_DRAIN_BYTES=32 MiB) and for SECONDS
# (default 180) w125_concurrent.py runs 8 writers, 8 verifying readers and
# 2 deleters (sizes 1 B - 4 MiB, packing at minimum age 2 s) next to a dd
# loop (1 GiB, bs 1M, conv=fsync). Before the read-headroom fix, reads
# failed ENOMEM (demand chunk buffers) and EBUSY (packed-read scratch) once
# pending writes filled the budget. Prints the client's read errors and the
# tool's verdict; exit 1 on any read error, mismatch or tool error.
set -u
REL=$1; OUT=$2; PORT=${3:-17700}; SEC=${4:-180}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-readp-run
mkdir -p "$OUT"
export EFS_DCACHE_HARD_BYTES=33554432 EFS_DCACHE_DRAIN_BYTES=33554432 EFS_PACK_MIN_AGE_MS=2000
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
"$HERE/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
head -c $((1 << 30)) /dev/urandom > $RUN/src1g; m=$(md5sum < $RUN/src1g | cut -c1-32)
( end=$(( $(date +%s) + SEC )); i=0; while [ "$(date +%s)" -lt $end ]; do
    dd if=$RUN/src1g of=$RUN/mnt/dd$((i % 3)) bs=1M conv=fsync status=none && \
    [ "$(md5sum < $RUN/mnt/dd$((i % 3)) | cut -c1-32)" = "$m" ] && echo "dd $i ok" || echo "dd $i FAIL"; i=$((i + 1)); done ) > "$OUT/dd.txt" 2>&1 &
python3 "$HERE/w125_concurrent.py" $RUN/mnt/c "$SEC" --writers 8 --readers 8 --deleters 2 > "$OUT/conc.txt" 2>&1; rc=$?
wait
grep -a "efs-fuse read:" $RUN/logs/fuse.log > "$OUT/read-errors.txt"
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
echo "read_errors=$(grep -c . "$OUT/read-errors.txt") enomem=$(grep -c 'out of memory' "$OUT/read-errors.txt") ebusy=$(grep -c 'resource busy' "$OUT/read-errors.txt") dd_fail=$(grep -c FAIL "$OUT/dd.txt") dd_ok=$(grep -c ok "$OUT/dd.txt")"
grep -E '^final' "$OUT/conc.txt"
[ $rc -eq 0 ] && [ ! -s "$OUT/read-errors.txt" ] && ! grep -q FAIL "$OUT/dd.txt"
