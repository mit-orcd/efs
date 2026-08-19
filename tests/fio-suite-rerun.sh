#!/bin/bash
# Faithful rerun of the fio-stagger suite (single host), direct=1.
# Same job set/order as the original: warmup then 9 jobs back-to-back.
set -u
MNT=${MNT:-/tmp/efs/mnt}
DIR=$MNT/fio/$(hostname -s)
SIZE=${SIZE:-512m}
SIZE4K=${SIZE4K:-256m}
RUNTIME=${RUNTIME:-12}
JOBS=${JOBS:-8}
OUT=${OUT:-/tmp/efs/fio-rerun}
mkdir -p "$DIR" "$OUT"
TSV=$OUT/results.tsv
: >"$TSV"

run_fio() {
    local name=$1 rw=$2 bs=$3 size=$4
    fio --name="$name" --directory="$DIR" --filename_format='f.$jobnum' \
        --rw="$rw" --bs="$bs" --size="$size" --numjobs="$JOBS" \
        --ioengine=psync --direct=1 --time_based --runtime="$RUNTIME" \
        --ramp_time=2 --group_reporting --randrepeat=0 \
        >"$OUT/$name.log" 2>&1
    local rc=$?
    local line
    line=$(grep -E '^\s+(read|write): IOPS=' "$OUT/$name.log" | head -1)
    local bw iops
    bw=$(echo "$line" | sed -E 's/.*BW=([0-9.]+)([KMG]?)iB\/s.*/\1 \2/')
    iops=$(echo "$line" | sed -E 's/.*IOPS=([0-9.k]+).*/\1/')
    echo -e "$(hostname -s)\t$name\t$bw\t$iops\t$rc" | tee -a "$TSV"
}

# Warmup: create/fill shared files (no time limit — full size).
fio --name=warmup-sw --directory="$DIR" --filename_format='f.$jobnum' \
    --rw=write --bs=1m --size="$SIZE" --numjobs="$JOBS" \
    --ioengine=psync --direct=1 --group_reporting \
    >"$OUT/warmup-sw.log" 2>&1
echo -e "$(hostname -s)\twarmup-sw\t$(grep -oE 'BW=[0-9.]+[KMG]?iB/s' "$OUT/warmup-sw.log" | head -1)\t-\t$?" | tee -a "$TSV"

SUITE=(
    "sw-1m write 1m $SIZE"
    "ow-1m write 1m $SIZE"
    "sr-1m read 1m $SIZE"
    "rw-1m randwrite 1m $SIZE"
    "rr-1m randread 1m $SIZE"
    "rw-128k randwrite 128k $SIZE"
    "rr-128k randread 128k $SIZE"
    "rw-4k randwrite 4k $SIZE4K"
    "rr-4k randread 4k $SIZE4K"
)
for job in "${SUITE[@]}"; do
    run_fio $job
done
echo "ALL_DONE $(date -Is)" | tee -a "$TSV"
