#!/bin/bash
# Seed fixed-size files for the 9-client read benchmark.
# Runs on each client against the local efs mount. direct=1 keeps the seed
# out of the client page cache so the later read phase hits the servers.
set -u
MNT=${MNT:-/tmp/efs/mnt-4n}
DIR=${DIR:-$MNT/fio/seed}
JOBS=${JOBS:-8}
SIZE=${SIZE:-512m}
BS=${BS:-1m}
OUT=${OUT:-/tmp/efs/fio-seed}
mkdir -p "$DIR" "$OUT"
rm -f "$OUT/done"
fio --name=seed --rw=write --bs="$BS" --iodepth=1 --numjobs="$JOBS" \
    --size="$SIZE" --group_reporting --direct=1 --ioengine=psync \
    --directory="$DIR" --filename_format='n.$jobnum' \
    --allow_file_create=1 --fallocate=none \
    >"$OUT/seed.log" 2>&1
echo "rc=$?" > "$OUT/done"
