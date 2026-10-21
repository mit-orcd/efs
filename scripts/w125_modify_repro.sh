#!/bin/bash
# w125_modify_repro.sh REL OUT [PORT] [SECONDS] [MIN_AGE_MS]
#
# W125 closure C1 on a private node9901 cluster (w125_p0_cluster.sh, ports
# PORT+1..PORT+3, default 17741-17743; never orcd-mini or w100): one client
# runs w125_concurrent.py for SECONDS (default 600) with 4 writers, 4
# verifying readers, 2 deleters and 4 modifiers (rewrite, append, truncate,
# overwrite in place), packer minimum age MIN_AGE_MS (default 3000). Output
# in OUT: a.txt (progress, final verify, first 20 errors), fuse.log. The
# cluster is stopped but its storage is kept in /tmp/w125-modify-PORT for
# inspection (w125_modify_inspect.py); the next run wipes it.
set -u
REL=$1; OUT=$2; PORT=${3:-17740}; SECS=${4:-600}; AGE=${5:-3000}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-modify-$PORT
mkdir -p "$OUT"
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_PACK_MIN_AGE_MS=$AGE
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
"$REL/scripts/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
python3 "$HERE/w125_concurrent.py" $RUN/mnt/c "$SECS" --writers 4 --readers 4 --deleters 2 --modifiers 4 > "$OUT/a.txt" 2>&1
echo "RC=$?" >> "$OUT/a.txt"
cp $RUN/logs/fuse.log "$OUT/fuse.log" 2>/dev/null
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1
echo DONE > "$OUT/done"
