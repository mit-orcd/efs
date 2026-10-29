#!/bin/bash
# orcd_test_ior.sh OUTDIR [MOUNT]
#
# IOR measurements on the orcd-test clients (fcstor007-013, efs-fuse mounted
# at MOUNT, default /tmp/efs-mount). Run on fcstor007 with an ssh agent that
# reaches every client (mpirun starts its daemons over ssh). Four cases, each
# writes with fsync on close (-e), reads back with data verification (-R) and
# deletes its files:
#   little-easy  1 rank,   file per process, 1 MiB transfers, 8 GiB
#   little-hard  1 rank,   shared file, 47,008-byte transfers, ~0.94 GB
#   wide-easy    7x8 ranks, file per process, 1 MiB transfers, 2 GiB each
#   wide-hard    7x8 ranks, shared file, 47,008-byte transfers, ~5 GB
# Wide cases reorder tasks for the read (-C) so no rank reads its own cache.
# OUTDIR gets one log per case and summary.tsv (case, rc, write/read MiB/s).
# IOR=, MPIVARS= and HOSTS= override the defaults below.
set -u
OUT=$1; MNT=${2:-/tmp/efs-mount}
IOR=${IOR:-$HOME/orcd/scratch/efs-io500/io500/bin/ior}
MPIVARS=${MPIVARS:-$(ls /usr/mpi/gcc/openmpi-*/bin/mpivars.sh 2>/dev/null | tail -1)}
HOSTS=${HOSTS:-fcstor007.ib,fcstor008.ib,fcstor009.ib,fcstor010.ib,fcstor011.ib,fcstor012.ib,fcstor013.ib}
PPN=${PPN:-8}
# mpivars.sh reads unset variables.
set +u
# shellcheck disable=SC1090
source "$MPIVARS"
set -u
mkdir -p "$OUT"
DIR=$MNT/ior-$(date -u +%Y%m%dT%H%M%S); mkdir -p "$DIR/easy" "$DIR/hard"
WIDE_HOSTS=$(echo "$HOSTS" | sed "s/,/:$PPN,/g; s/\$/:$PPN/")
NWIDE=$(( $(echo "$HOSTS" | tr , '\n' | wc -l) * PPN ))
printf 'case\trc\twrite_MiBps\tread_MiBps\n' > "$OUT/summary.tsv"
run() { # case np hosts ior-args...
    local name=$1 np=$2 hosts=$3; shift 3
    echo "=== $name np=$np $(date -u +%T)"
    timeout 3600 mpirun -np "$np" --host "$hosts" --map-by node --bind-to none \
        "$IOR" -i 1 -w -r -R -e -g "$@" > "$OUT/$name.log" 2>&1; local rc=$?
    local w r
    w=$(awk '$1=="write" && $2+0>0 {print $2; exit}' "$OUT/$name.log")
    r=$(awk '$1=="read" && $2+0>0 {print $2; exit}' "$OUT/$name.log")
    printf '%s\t%s\t%s\t%s\n' "$name" "$rc" "${w:--}" "${r:--}" >> "$OUT/summary.tsv"
    echo "$name rc=$rc write=${w:--} read=${r:--} MiB/s"
    grep -iE 'error|verif|mismatch' "$OUT/$name.log" | head -5
}
H1=${HOSTS%%,*}
run little-easy 1 "$H1" -F -t 1m -b 8g -o "$DIR/easy/little"
run little-hard 1 "$H1" -t 47008 -b 47008 -s 20000 -o "$DIR/hard/little"
run wide-easy "$NWIDE" "$WIDE_HOSTS" -F -C -t 1m -b 2g -o "$DIR/easy/wide"
run wide-hard "$NWIDE" "$WIDE_HOSTS" -C -t 47008 -b 47008 -s 1900 -o "$DIR/hard/wide"
rmdir "$DIR/easy" "$DIR/hard" "$DIR" 2>/dev/null
awk -F'\t' 'NR>1 && $2!=0 {bad++} END {print (bad ? "FAIL " bad : "PASS all"), "cases"; exit (bad ? 1 : 0)}' "$OUT/summary.tsv"
