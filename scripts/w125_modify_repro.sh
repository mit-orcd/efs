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
# final verify, first 20 errors), fuse.log, stop.txt (the checked stop's
# report) and, for each mismatching read, the bytes it returned under
# mismatch/N/NAME with mismatch/N/meta.json (expected version, lengths,
# hashes, inode, read time, capture hashes) and the server's
# raft-getattr/raft-getchunks output captured at that moment; a copy of the
# file taken under the workload's per-file lock is kept under RUN/mnt/keep/N
# (exclusive-create, never a hard link, so a later modifier cannot change
# the captured bytes). OUT must not exist. Inspect the bytes with
#   scripts/w125_modify_inspect.py OUT/mismatch/N NAME
#
# The store is deleted only after PASS and a clean stop (w125_p0_cluster.sh
# stop verified the unmount and every daemon's orderly exit with status 0).
# After any failure, or a forced or unverified stop, it is kept and its
# path is printed; `done` says which. Raw output stays outside Git.
#
# Exit status: the workload's (0 PASS, 1 mismatch or error), 2 when the
# cluster does not start or OUT/RUN cannot be set up, 3 when the workload
# passed but the stop was forced or unverified (store kept), 4 when the
# store of a passing run could not be deleted. Every cleanup status is
# recorded in run.txt and done.
#
# W125_WORKLOAD=PATH replaces w125_concurrent.py (harness self-tests only).
set -u
REL=$1; OUT=$2; PORT=${3:-17740}; SECS=${4:-600}; AGE=${5:-3000}
HERE=$(cd "$(dirname "$0")" && pwd)
WORKLOAD=${W125_WORKLOAD:-$HERE/w125_concurrent.py}
RUN=${W125_RUN:-/tmp/w125-modify-$PORT-$(date -u +%Y%m%dT%H%M%SZ)}
[ -e "$RUN" ] && { echo "w125_modify_repro: $RUN exists; refusing to reuse it" >&2; exit 2; }
[ -e "$OUT" ] && { echo "w125_modify_repro: $OUT exists; refusing to reuse it" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_PACK_MIN_AGE_MS=$AGE
sha() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
{
    echo "run=$RUN port=$PORT seconds=$SECS min_age_ms=$AGE rel=$REL"
    echo "started=$(date -u +%FT%TZ)"
    (cd "$REL" && sha efsd efs-fuse) 2>&1
} > "$OUT/run.txt" || exit 2
if ! "$REL/scripts/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/start.txt" 2>&1; then
    cat "$OUT/start.txt"
    "$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > "$OUT/stop.txt" 2>&1
    stop_rc=$?
    echo "finished=$(date -u +%FT%TZ) start=failed stop_rc=$stop_rc" >> "$OUT/run.txt"
    echo "FAIL start failed stop_rc=$stop_rc store=$RUN" > "$OUT/done"
    echo "w125_modify_repro: start failed (stop rc=$stop_rc); store kept in $RUN" >&2
    exit 2
fi
python3 "$WORKLOAD" "$RUN/mnt/c" "$SECS" --writers 4 --readers 4 --deleters 2 \
    --modifiers 4 --mismatch-dir "$OUT/mismatch" --keep-dir "$RUN/mnt/keep" \
    --mgmt "$REL/efs-mgmt ${W125_ADDR:-10.1.99.1}:$((PORT+1))" > "$OUT/a.txt" 2>&1
rc=$?
echo "RC=$rc" >> "$OUT/a.txt"
cp "$RUN/logs/fuse.log" "$OUT/fuse.log" 2>/dev/null || echo "fuse.log: not copied" >> "$OUT/run.txt"
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > "$OUT/stop.txt" 2>&1
stop_rc=$?
# The helper verified the unmount; this is a second, independent check
# before anything is deleted.
if command -v findmnt > /dev/null 2>&1; then findmnt -rn "$RUN/mnt" > /dev/null 2>&1 && stop_rc=3
else mount | grep -q " on $RUN/mnt " && stop_rc=3; fi
stop_kind=clean; [ $stop_rc -ne 0 ] && stop_kind=forced
echo "finished=$(date -u +%FT%TZ) workload_rc=$rc stop_rc=$stop_rc stop=$stop_kind" >> "$OUT/run.txt"
if [ $rc -eq 0 ] && [ $stop_rc -eq 0 ]; then
    if [ -x "$HOME/bin/edelete" ]; then "$HOME/bin/edelete" --force --delete "$RUN" > "$OUT/delete.txt" 2>&1
    else rm -rf "$RUN" > "$OUT/delete.txt" 2>&1; fi
    del_rc=$?
    [ -e "$RUN" ] && [ $del_rc -eq 0 ] && del_rc=1
    echo "delete_rc=$del_rc" >> "$OUT/run.txt"
    if [ $del_rc -ne 0 ]; then
        echo "PASS stop=clean delete_rc=$del_rc store=$RUN (not deleted)" > "$OUT/done"
        echo "w125_modify_repro: PASS, but the store could not be deleted (rc=$del_rc); left in $RUN" >&2
        exit 4
    fi
    echo "PASS stop=clean store deleted" > "$OUT/done"
    exit 0
fi
echo "FAIL workload_rc=$rc stop_rc=$stop_rc stop=$stop_kind store=$RUN" > "$OUT/done"
echo "w125_modify_repro: FAIL (workload rc=$rc, stop rc=$stop_rc, stop $stop_kind); store kept in $RUN" >&2
[ $rc -ne 0 ] && exit $rc
exit 3
