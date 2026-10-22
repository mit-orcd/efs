#!/bin/bash
# k08_stress_repro.sh REL OUT PORT [SECONDS] [STRESS_ARGS...]
#
# W148 / alpha K8: scripts/k08_overwrite_extend.py on a private
# w125_p0_cluster.sh cluster (RDMA mlx5_0, --direct-io, ports PORT+1..PORT+3;
# never orcd-mini 17432-17434 or w100 27432/27433). OUT must not exist. The
# run directory is /tmp/k08-stress-PORT-<UTC stamp>. OUT gets run.txt
# (arguments, binary sha256), a.txt (stressor output), fuse.log and, for
# each mismatching read, dump/N/{actual,expected,meta.json}. PASS deletes
# the stopped store with ~/bin/edelete; any failure keeps it.
# K08_LOAD=1 also runs scripts/w125_concurrent.py (4 writers, 4 readers,
# 2 deleters, no modifiers) on the same mount for the same time, so pressure
# flushes, whole-set background REPORTs and the packer run as in the W125
# modify repro; its output goes to OUT/load.txt and its mismatches under
# OUT/load-mismatch, and its failure fails the run.
# Exit: the stressor's status (or 1 for a failed load), 2 when the cluster
# does not start.
set -u
REL=$1; OUT=$2; PORT=$3; SECS=${4:-600}; shift 4 2>/dev/null || shift $#
HERE=$(cd "$(dirname "$0")" && pwd)
RUN=/tmp/k08-stress-$PORT-$(date -u +%Y%m%dT%H%M%SZ)
[ -e "$OUT" ] && { echo "k08_stress_repro: $OUT exists; refusing to reuse it" >&2; exit 2; }
[ -e "$RUN" ] && { echo "k08_stress_repro: $RUN exists" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
{
    echo "run=$RUN port=$PORT seconds=$SECS rel=$REL load=${K08_LOAD:-0} args=$*"
    echo "started=$(date -u +%FT%TZ)"
    (cd "$REL" && sha256sum efsd efs-fuse && cat SOURCE_COMMIT 2>/dev/null)
} > "$OUT/run.txt" 2>&1
if ! "$REL/scripts/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/start.txt" 2>&1; then
    "$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1
    echo "FAIL start store=$RUN" > "$OUT/done"; exit 2
fi
lpid=
if [ "${K08_LOAD:-0}" = 1 ]; then
    python3 "$HERE/w125_concurrent.py" "$RUN/mnt/load" "$SECS" --writers 4 --readers 4 \
        --deleters 2 --mismatch-dir "$OUT/load-mismatch" > "$OUT/load.txt" 2>&1 &
    lpid=$!
fi
python3 "$HERE/k08_overwrite_extend.py" "$RUN/mnt/k08" "$SECS" --dump "$OUT/dump" "$@" > "$OUT/a.txt" 2>&1
rc=$?
if [ -n "$lpid" ]; then
    wait "$lpid"; lrc=$?
    echo "load_rc=$lrc" >> "$OUT/run.txt"
    [ $rc -eq 0 ] && [ $lrc -ne 0 ] && rc=1
fi
cp "$RUN/logs/fuse.log" "$OUT/fuse.log" 2>/dev/null
"$REL/scripts/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > "$OUT/stop.txt" 2>&1
sleep 2
mounted=0; findmnt -rn "$RUN/mnt" > /dev/null 2>&1 && mounted=1
echo "finished=$(date -u +%FT%TZ) rc=$rc mounted_after_stop=$mounted" >> "$OUT/run.txt"
if [ $rc -eq 0 ] && [ $mounted -eq 0 ]; then
    ~/bin/edelete --force --delete "$RUN" > /dev/null 2>&1
    echo PASS > "$OUT/done"; exit 0
fi
echo "FAIL rc=$rc mounted_after_stop=$mounted store=$RUN" > "$OUT/done"
exit $([ $rc -ne 0 ] && echo $rc || echo 3)
