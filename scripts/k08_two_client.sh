#!/bin/bash
# k08_two_client.sh REL OUT PORT [FILTER] [REPEAT]
#
# W148 / alpha K8: tests/posix/posix_2client.py --local (from THIS tree)
# against the binaries in REL, on a private w125_p0_cluster.sh cluster
# (RDMA mlx5_0, --direct-io, ports PORT+1..PORT+3; never orcd-mini
# 17432-17434 or w100 27432/27433) with two efs-fuse mounts, RUN/mnt (A)
# and RUN/mntb (B). FILTER (default peer_partial_after_full_overwrite) is
# run REPEAT times (default 3), each under its own parent directory. OUT
# must not exist; it gets run.txt (binary sha256), rN.out and rN.tsv. PASS
# deletes the stopped store with ~/bin/edelete; a failure keeps it.
# Exit: 0 when every repetition passed, 1 otherwise, 2 when the cluster
# does not start.
set -u
REL=$1; OUT=$2; PORT=$3; FILTER=${4:-peer_partial_after_full_overwrite}; REPEAT=${5:-3}
HERE=$(cd "$(dirname "$0")" && pwd); T=$(dirname "$HERE")/tests
ADDR=${W125_ADDR:-10.1.99.1}
RUN=/tmp/k08-2c-$PORT-$(date -u +%Y%m%dT%H%M%SZ)
[ -e "$OUT" ] && { echo "k08_two_client: $OUT exists; refusing to reuse it" >&2; exit 2; }
mkdir -p "$OUT" || exit 2
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
{ echo "run=$RUN port=$PORT rel=$REL filter=$FILTER repeat=$REPEAT tests=$T"
  (cd "$REL" && sha256sum efsd efs-fuse && cat SOURCE_COMMIT 2>/dev/null); } > "$OUT/run.txt" 2>&1
if ! "$HERE/w125_p0_cluster.sh" start "$REL" "$RUN" "$PORT" > "$OUT/start.txt" 2>&1; then
    "$HERE/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > /dev/null 2>&1
    echo "FAIL start store=$RUN" > "$OUT/done"; exit 2
fi
mkdir -p "$RUN/mntb"
EFS_FUSE_LOG="$RUN/logs/fuse-b.log" "$REL/scripts/client.sh" "$ADDR:$((PORT+1))" "$RUN/mntb" > "$RUN/logs/client-b.out" 2>&1
rc=0
for i in $(seq 1 "$REPEAT"); do
    EFS_POSIX2_PARENT=k08-2c-$i python3 "$T/posix/posix_2client.py" --local "$RUN/mnt" "$RUN/mntb" \
        --filter "$FILTER" --results "$OUT/r$i.tsv" > "$OUT/r$i.out" 2>&1
    r=$?; [ $r -ne 0 ] && rc=1
    echo "repetition $i rc=$r $(tail -1 "$OUT/r$i.out")" >> "$OUT/run.txt"
done
cp "$RUN/logs/fuse.log" "$OUT/fuse-a.log" 2>/dev/null
cp "$RUN/logs/fuse-b.log" "$OUT/fuse-b.log" 2>/dev/null
"$REL/scripts/client.sh" stop "$RUN/mntb" > /dev/null 2>&1
"$HERE/w125_p0_cluster.sh" stop "$REL" "$RUN" "$PORT" > "$OUT/stop.txt" 2>&1
sleep 2
mounted=0
for m in "$RUN/mnt" "$RUN/mntb"; do findmnt -rn "$m" > /dev/null 2>&1 && mounted=1; done
echo "finished=$(date -u +%FT%TZ) rc=$rc mounted_after_stop=$mounted" >> "$OUT/run.txt"
if [ $rc -eq 0 ] && [ $mounted -eq 0 ]; then
    ~/bin/edelete --force --delete "$RUN" > /dev/null 2>&1
    echo PASS > "$OUT/done"; exit 0
fi
echo "FAIL rc=$rc mounted_after_stop=$mounted store=$RUN" > "$OUT/done"
exit 1
