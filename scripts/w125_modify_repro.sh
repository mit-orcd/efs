#!/bin/bash
# w125_modify_repro.sh REL OUT [PORT] [SECONDS] [MIN_AGE_MS]
#
# W125 closure C1 on a private node9901 cluster (w125_p0_cluster.sh, ports
# PORT+1..PORT+3, default 17741-17743; never orcd-mini or w100): one client
# runs w125_concurrent.py for SECONDS (default 600) with 4 writers, 4
# verifying readers, 2 deleters and 4 modifiers (rewrite, append, truncate,
# overwrite in place), packer minimum age MIN_AGE_MS (default 3000).
#
# Every attempt gets its own run directory, /tmp/w125-modify-PORT-<UTC stamp>
# (or $W125_RUN), so a later run never removes an earlier one's store. OUT
# gets run.txt (run directory, arguments, binary sha256), a.txt (progress,
# final verify, first 20 errors), fuse.log and, for each mismatching read,
# the bytes it returned under mismatch/N/NAME; inspect them with
#   scripts/w125_modify_inspect.py OUT/mismatch/N NAME
# On PASS the stopped store is deleted; on any failure it is kept for
# inspection and its path is printed. Raw output stays outside Git.
#
# Exit status: the workload's (0 PASS, 1 mismatch or error), 2 when the
# cluster does not start, 3 when the workload passed but the stop failed.
set -u
REL=$1; OUT=$2; PORT=${3:-17740}; SECS=${4:-600}; AGE=${5:-3000}
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=${W125_RUN:-/tmp/w125-modify-$PORT-$(date -u +%Y%m%dT%H%M%SZ)}
[ -e "$RUN" ] && { echo "w125_modify_repro: $RUN exists; refusing to reuse it" >&2; exit 2; }
mkdir -p "$OUT"
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_PACK_MIN_AGE_MS=$AGE
{
    echo "run=$RUN port=$PORT seconds=$SECS min_age_ms=$AGE rel=$REL"
    echo "started=$(date -u +%FT%TZ)"
    (cd "$REL" && sha256sum efsd efs-fuse) 2>&1
} > "$OUT/run.txt"
if ! "$REL/scripts/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/start.txt" 2>&1; then
    cat "$OUT/start.txt"
    "$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1
    echo "w125_modify_repro: start failed; store kept in $RUN" >&2
    exit 2
fi
python3 "$HERE/w125_concurrent.py" "$RUN/mnt/c" "$SECS" --writers 4 --readers 4 --deleters 2 \
    --modifiers 4 --mismatch-dir "$OUT/mismatch" > "$OUT/a.txt" 2>&1
rc=$?
echo "RC=$rc" >> "$OUT/a.txt"
cp "$RUN/logs/fuse.log" "$OUT/fuse.log" 2>/dev/null
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > "$OUT/stop.txt" 2>&1
stop_rc=$?
echo "finished=$(date -u +%FT%TZ) workload_rc=$rc stop_rc=$stop_rc" >> "$OUT/run.txt"
if [ $rc -eq 0 ] && [ $stop_rc -eq 0 ]; then
    rm -rf "$RUN"
    echo PASS > "$OUT/done"
    exit 0
fi
echo "FAIL workload_rc=$rc stop_rc=$stop_rc store=$RUN" > "$OUT/done"
echo "w125_modify_repro: FAIL (workload rc=$rc, stop rc=$stop_rc); store kept in $RUN" >&2
[ $rc -ne 0 ] && exit $rc
exit 3
