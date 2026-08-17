#!/bin/bash
# 50-round parallel fio from fcstor007-015 against a live efs FUSE mount.
# Run from the Engaging login node. Does not compile.
set -euo pipefail
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
HOSTS=(fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib
       fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib)
MNT=/tmp/efs/mnt-4n
ROUNDS=${1:-50}
RUNTIME=${RUNTIME:-8}
JOBS=${JOBS:-4}
SIZE=${SIZE:-256m}
BS=${BS:-1m}
OUTDIR=${OUTDIR:-/tmp/efs-fio-9way}
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/summary.tsv"
echo -e "round\tphase\thosts_ok\tagg_mib_s\tmin_mib_s\tmax_mib_s\terrors" > "$SUMMARY"

fio_one() {
    local host=$1 phase=$2 rw=$3 round=$4
    local dir="$MNT/fio/r${round}/$host"
    $SSH "$host" "mkdir -p '$dir' && fio --name=$phase --rw=$rw --bs=$BS --iodepth=1 \
        --numjobs=$JOBS --size=$SIZE --time_based --runtime=$RUNTIME \
        --group_reporting --direct=0 --ioengine=psync \
        --directory='$dir' --filename_format='n.\$jobnum' \
        --output-format=terse --terse-version=3" 2>/dev/null
}

parse_bw() {
    # fio terse v3: write bw is field 48 (KiB/s), read bw is field 7 (KiB/s)
    # https://fio.readthedocs.io/en/latest/fio_doc.html#terse-output
    local line=$1 phase=$2
    if [ "$phase" = write ]; then
        echo "$line" | awk -F';' '{print $48+0}'
    else
        echo "$line" | awk -F';' '{print $7+0}'
    fi
}

for r in $(seq 1 "$ROUNDS"); do
    for phase_rw in "write:write" "read:read"; do
        phase=${phase_rw%%:*}
        rw=${phase_rw##*:}
        tmp="$OUTDIR/r${r}.${phase}"
        rm -f "$tmp".*
        ok=0
        err=0
        declare -a bws=()
        for h in "${HOSTS[@]}"; do
            (
                out=$(fio_one "$h" "$phase" "$rw" "$r" || true)
                bw=$(parse_bw "$out" "$phase")
                if [ -z "$bw" ] || [ "$bw" = "0" ] && ! echo "$out" | grep -q ';'; then
                    echo "FAIL $h" > "$tmp.$h"
                else
                    echo "$bw" > "$tmp.$h"
                fi
            ) &
        done
        wait
        sum=0
        min=999999999
        max=0
        for h in "${HOSTS[@]}"; do
            if [ ! -f "$tmp.$h" ]; then
                err=$((err+1))
                continue
            fi
            val=$(cat "$tmp.$h")
            if [ "$val" = "FAIL $h" ] || ! [[ "$val" =~ ^[0-9]+$ ]]; then
                err=$((err+1))
                continue
            fi
            ok=$((ok+1))
            sum=$((sum+val))
            [ "$val" -lt "$min" ] && min=$val
            [ "$val" -gt "$max" ] && max=$val
        done
        # KiB/s -> MiB/s
        agg=$(awk -v s="$sum" 'BEGIN{printf "%.1f", s/1024}')
        minm=$(awk -v s="$min" 'BEGIN{printf "%.1f", (s>=999999999)?0:s/1024}')
        maxm=$(awk -v s="$max" 'BEGIN{printf "%.1f", s/1024}')
        echo -e "$r\t$phase\t$ok\t$agg\t$minm\t$maxm\t$err" | tee -a "$SUMMARY"
    done
done
echo "wrote $SUMMARY"
