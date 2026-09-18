#!/bin/bash
# Local-NVMe ceiling suite. Runs ON one efsd server against /data1/01..06.
#
# Writes only under <path>/fio-ceil — never touches the live <path>/efs tree
# that efsd is using. After the run, callers must rm -rf those fio-ceil dirs.
#
# Usage: perf_local_nvme.sh <out-tsv> [quick|full] [serial|parallel|both]
#
#   serial   one drive at a time (per-device ceiling)
#   parallel all 6 drives at once (host aggregate ceiling)
#   both     serial then parallel (default)
#
# Output TSV columns:
#   ts  host  path  layout  suite  test  bw_mib_s  iops  rc
set -u
OUT=${1:?usage: perf_local_nvme.sh <out-tsv> [quick|full] [serial|parallel|both]}
MODE=${2:-full}
LAYOUT=${3:-both}
HERE=$(cd "$(dirname "$0")" && pwd)
HOST=$(hostname -s)
PATHS=(/data1/01 /data1/02 /data1/03 /data1/04 /data1/05 /data1/06)
CEIL=fio-ceil

mkdir -p "$(dirname "$OUT")"
echo -e "ts\thost\tpath\tlayout\tsuite\ttest\tbw_mib_s\tiops\trc" >"$OUT"

run_one() { # layout path
    local layout=$1 p=$2
    local mnt="$p/$CEIL"
    local raw="/tmp/perf-nvme-$HOST-${p##*/}-$layout.tsv"
    mkdir -p "$mnt"
    rm -f "$raw"
    bash "$HERE/device_fio.sh" "$mnt" "$raw" "$MODE"
    awk -v h="$HOST" -v p="$p" -v L="$layout" -F'\t' '
        NR==1 { next }
        $1 ~ /^PERF_NODE_DONE/ { next }
        NF>=7 { print $1"\t"h"\t"p"\t"L"\t"$3"\t"$4"\t"$5"\t"$6"\t"$7 }
    ' "$raw" >>"$OUT"
    rm -f "$raw"
}

run_serial() {
    local p
    for p in "${PATHS[@]}"; do
        echo "[perf_local_nvme] $HOST serial $p" >&2
        run_one serial "$p"
        rm -rf "$p/$CEIL"
    done
}

run_parallel() {
    local p pids=()
    echo "[perf_local_nvme] $HOST parallel ${PATHS[*]}" >&2
    for p in "${PATHS[@]}"; do
        ( run_one parallel "$p" ) &
        pids+=($!)
    done
    local rc=0
    for pid in "${pids[@]}"; do
        wait "$pid" || rc=1
    done
    for p in "${PATHS[@]}"; do
        rm -rf "$p/$CEIL"
    done
    return $rc
}

case "$LAYOUT" in
    serial)   run_serial ;;
    parallel) run_parallel ;;
    both)     run_serial; run_parallel ;;
    *) echo "unknown layout: $LAYOUT" >&2; exit 2 ;;
esac
echo "PERF_LOCAL_NVME_DONE host=$HOST mode=$MODE layout=$LAYOUT $(date -Is)" >>"$OUT"
