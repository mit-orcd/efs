#!/bin/bash
# Run N fio write+read rounds against the local efs FUSE mount.
# Intended to run on fcstor007-015 in parallel.
MNT=/tmp/efs/mnt-4n
HOST=$(hostname -s)
ROUNDS=${1:-50}
RUNTIME=${RUNTIME:-8}
JOBS=${JOBS:-4}
SIZE=${SIZE:-256m}
BS=${BS:-1m}
OUT=/tmp/efs/fio-rounds
mkdir -p "$OUT"
LOG=$OUT/rounds.tsv
ERRLOG=$OUT/errors.log
: > "$ERRLOG"
echo -e "round\tphase\tmib_s\tiops\texit\tfuse_err" > "$LOG"

fuse_err_count() {
    local n
    n=$(grep -cE 'EIO|no quorum|checksum|PUT_META|writeback|Input/output' \
        /tmp/efs/efs-fuse-mnt-4n.log 2>/dev/null) || n=0
    printf '%s\n' "${n:-0}"
}

run_one() {
    local r=$1 phase=$2 rw=$3
    local dir="$MNT/fio/${HOST}/r${r}"
    mkdir -p "$dir" || return 2
    local before after rc mib iops
    before=$(fuse_err_count)
    local flog="$OUT/r${r}.${phase}.log"
    fio --name="$phase" --rw="$rw" --bs="$BS" --iodepth=1 --numjobs="$JOBS" \
        --size="$SIZE" --time_based --runtime="$RUNTIME" --group_reporting \
        --direct=0 --ioengine=psync --directory="$dir" \
        --filename_format='n.$jobnum' --allow_file_create=1 \
        >"$flog" 2>&1
    rc=$?
    after=$(fuse_err_count)
    local newerr=$((after - before))
    mib=$(awk '
        /WRITE:|write:|READ:|read:/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^(bw|BW)=/) { print $i; exit }
            }
        }' "$flog" \
          | sed -e 's/^[bB][wW]=//' -e 's/MiB\/s.*//' -e 's/KiB\/s.*/\/1024/' )
    # normalize units: if KiB/s the sed left "N/1024"
    if echo "$mib" | grep -q /; then
        mib=$(awk -v x="$mib" 'BEGIN{split(x,a,"/"); printf "%.1f", a[1]/a[2]}')
    fi
    iops=$(awk '/WRITE:|write:|READ:|read:/ {
                    for (i = 1; i <= NF; i++)
                        if ($i ~ /^(iops|IOPS)=/) { print $i; exit }
                }' "$flog" | sed -e 's/^[iI][oO][pP][sS]=//' -e 's/,.*//')
    [ -z "$mib" ] && mib=0
    [ -z "$iops" ] && iops=0
    echo -e "$r\t$phase\t$mib\t$iops\t$rc\t$newerr" | tee -a "$LOG"
    if [ "$rc" -ne 0 ] || [ "$newerr" -gt 0 ]; then
        echo "ROUND $r $phase rc=$rc fuse_err=$newerr" >> "$ERRLOG"
        tail -20 "$flog" >> "$ERRLOG"
        tail -20 /tmp/efs/efs-fuse-mnt-4n.log >> "$ERRLOG"
    fi
    return 0
}

echo "START host=$HOST rounds=$ROUNDS $(date -Is)" | tee "$OUT/start"
for r in $(seq 1 "$ROUNDS"); do
    run_one "$r" write write
    run_one "$r" read read
    echo "DONE_ROUND $r $(date +%s)" > "$OUT/progress"
done
echo "ALL_DONE host=$HOST $(date -Is)" | tee "$OUT/done"
