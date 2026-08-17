#!/bin/bash
# One-shot usual fio suite on a local efs FUSE mount.
# Client index rotates the starting job so 9 hosts overlap different IO.
set -u
MNT=/tmp/efs/mnt-4n
HOST=$(hostname -s)
# fcstor007 -> 0 ... fcstor015 -> 8
IDX=${IDX:-0}
case "$HOST" in
    fcstor007) IDX=0 ;;
    fcstor008) IDX=1 ;;
    fcstor009) IDX=2 ;;
    fcstor010) IDX=3 ;;
    fcstor011) IDX=4 ;;
    fcstor012) IDX=5 ;;
    fcstor013) IDX=6 ;;
    fcstor014) IDX=7 ;;
    fcstor015) IDX=8 ;;
esac
RUNTIME=${RUNTIME:-15}
JOBS=${JOBS:-8}
SIZE=${SIZE:-512m}
SIZE4K=${SIZE4K:-256m}
OUT=/tmp/efs/fio-stagger
DIR="$MNT/fio/${HOST}"
mkdir -p "$OUT" "$DIR"
if [ "${1:-}" = "sync-read" ]; then
    LOG=$OUT/sync.tsv
else
    LOG=$OUT/results.tsv
fi
echo -e "host\tidx\tjob\tmib_s\tiops\texit" > "$LOG"
echo "START host=$HOST idx=$IDX mode=${1:-suite} $(date -Is)" | tee "$OUT/start"

parse() {
    local flog=$1
    awk '
        /WRITE:|write:|READ:|read:/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^(bw|BW)=/) bw=$i
                if ($i ~ /^(iops|IOPS)=/) iops=$i
            }
        }
        END {
            gsub(/^[bB][wW]=/, "", bw)
            gsub(/MiB\/s.*/, "", bw)
            if (bw ~ /KiB/) { gsub(/KiB\/s.*/, "", bw); bw=bw/1024 }
            gsub(/^[iI][oO][pP][sS]=/, "", iops)
            gsub(/,.*/, "", iops)
            if (bw=="") bw=0
            if (iops=="") iops=0
            print bw, iops
        }' "$flog"
}

run_fio() {
    local name=$1 rw=$2 bs=$3 sz=$4 extra=${5:-}
    local flog="$OUT/${name}.log"
    # shellcheck disable=SC2086
    fio --name="$name" --rw="$rw" --bs="$bs" --iodepth=1 --numjobs="$JOBS" \
        --size="$sz" --time_based --runtime="$RUNTIME" --group_reporting \
        --direct=0 --ioengine=psync --directory="$DIR" \
        --filename_format='n.$jobnum' --allow_file_create=1 --fallocate=none \
        --end_fsync=0 --invalidate=1 $extra \
        >"$flog" 2>&1
    local rc=$?
    local bw iops
    read -r bw iops < <(parse "$flog")
    echo -e "$HOST\t$IDX\t$name\t$bw\t$iops\t$rc" | tee -a "$LOG"
}

if [ "${1:-}" = "sync-read" ]; then
    run_fio sr-1m-sync read 1m "$SIZE"
    echo "SYNC_DONE host=$HOST $(date -Is)" | tee "$OUT/sync_done"
    exit 0
fi

# Stagger start so opens/creates do not align.
sleep $((IDX * 2))

# Warmup: create/fill files so later reads and overwrites have data.
run_fio warmup-sw write 1m "$SIZE"

# Usual suite, rotated so each client starts on a different job.
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
n=${#SUITE[@]}
for k in $(seq 0 $((n - 1))); do
    i=$(( (IDX + k) % n ))
    # shellcheck disable=SC2086
    run_fio ${SUITE[$i]}
done

echo "ALL_DONE host=$HOST $(date -Is)" | tee "$OUT/done"
