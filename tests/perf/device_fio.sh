#!/bin/bash
# dd + fio against a LOCAL block device / filesystem. Runs ON a single node
# and appends result rows to $OUT (TSV). Used by perf_local_nvme.sh to
# measure the /data1/01-06 NVMe ceiling.
#
# NOT for efs: the fio jobs are time_based with no end_fsync, which on efs
# measures the client's userspace dcache, not the filesystem. Use
# tests/stress/fio_honest_matrix.sh for efs.
#
# Usage: device_fio.sh <dir> <out-tsv> [quick|full]
#   dir  : local filesystem dir to test (e.g. /data1/01/fio-ceil)
#   out  : results TSV path
#   mode : quick (small sizes, short runtimes) | full (default)
#
# Env overrides: DD_SIZE_MIB, FIO_SIZE, FIO_SIZE4K, FIO_RUNTIME, FIO_JOBS.
set -u
MNT=${1:?usage: device_fio.sh <dir> <out-tsv> [quick|full]}
OUT=${2:?usage: device_fio.sh <dir> <out-tsv> [quick|full]}
MODE=${3:-full}

HOST=$(hostname -s)
DIR="$MNT/perf/$HOST"
# Logs go to /tmp, never under $DIR: the harness must not depend on the very
# path it is benchmarking.
LOGDIR=$(mktemp -d "/tmp/efs-dev-fio-$HOST.XXXXXX")
trap 'rm -rf "$LOGDIR"' EXIT
mkdir -p "$DIR" "$(dirname "$OUT")"

if [ "$MODE" = quick ]; then
    DD_SIZE_MIB=${DD_SIZE_MIB:-512}
    FIO_SIZE=${FIO_SIZE:-256m}
    FIO_SIZE4K=${FIO_SIZE4K:-128m}
    FIO_RUNTIME=${FIO_RUNTIME:-6}
else
    DD_SIZE_MIB=${DD_SIZE_MIB:-2048}
    FIO_SIZE=${FIO_SIZE:-512m}
    FIO_SIZE4K=${FIO_SIZE4K:-256m}
    FIO_RUNTIME=${FIO_RUNTIME:-12}
fi
FIO_JOBS=${FIO_JOBS:-8}
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

[ -s "$OUT" ] || echo -e "ts\thost\tsuite\ttest\tbw_mib_s\tiops\trc" >>"$OUT"
record() { echo -e "$TS\t$HOST\t$1\t$2\t$3\t$4\t$5" >>"$OUT"; }

# "NNN MB/s" | "N.N GB/s" -> MiB/s
tomib() {
    local num=${1% *} unit=${1#* }
    case "$unit" in
        GB/s) awk -v n="$num" 'BEGIN{printf "%.1f", n*1024}' ;;
        MB/s) awk -v n="$num" 'BEGIN{printf "%.1f", n}' ;;
        *)    echo "0" ;;
    esac
}
# fio "BW=NNN<K|M|G>iB/s" -> MiB/s ; input here is "NNN U" (U in K/M/G)
fiomib() {
    local num=${1% *} unit=${1#* }
    case "$unit" in
        K) awk -v n="$num" 'BEGIN{printf "%.1f", n/1024}' ;;
        M) awk -v n="$num" 'BEGIN{printf "%.1f", n}' ;;
        G) awk -v n="$num" 'BEGIN{printf "%.1f", n*1024}' ;;
        *) echo "0" ;;
    esac
}

# --- dd: sequential write + read back, direct and cached ---
dd_test() { # name bs count oflag
    local name=$1 bs=$2 count=$3 oflag=$4 iflag
    local f="$DIR/dd.$name"
    iflag=${oflag/oflag/iflag}
    dd if=/dev/zero of="$f" bs="$bs" count="$count" $oflag \
        >"$LOGDIR/dd.$name.w.log" 2>&1
    local wrc=$?
    dd if="$f" of=/dev/null bs="$bs" $iflag >"$LOGDIR/dd.$name.r.log" 2>&1
    local rrc=$?
    local wr rd
    wr=$(grep -oE '[0-9.]+ [KMG]B/s' "$LOGDIR/dd.$name.w.log" | tail -1)
    rd=$(grep -oE '[0-9.]+ [KMG]B/s' "$LOGDIR/dd.$name.r.log" | tail -1)
    rm -f "$f"
    record dd "$name.write" "$(tomib "$wr")" "-" "$wrc"
    record dd "$name.read"  "$(tomib "$rd")" "-" "$rrc"
}

dd_test seq1m        1M "$DD_SIZE_MIB" "oflag=direct"
dd_test seq1m_cached 1M "$DD_SIZE_MIB" ""

# --- fio suite (warmup + 9 jobs), same shape as fio-suite-rerun.sh ---
run_fio() { # name rw bs size
    local name=$1 rw=$2 bs=$3 size=$4
    fio --name="$name" --directory="$DIR" --filename_format='f.$jobnum' \
        --rw="$rw" --bs="$bs" --size="$size" --numjobs="$FIO_JOBS" \
        --ioengine=psync --direct=1 --time_based --runtime="$FIO_RUNTIME" \
        --ramp_time=2 --group_reporting --randrepeat=0 \
        >"$LOGDIR/$name.log" 2>&1
    local rc=$? line bw iops
    line=$(grep -E '^\s+(read|write): IOPS=' "$LOGDIR/$name.log" | head -1)
    bw=$(echo "$line"  | sed -E 's/.*BW=([0-9.]+)([KMG])iB\/s.*/\1 \2/')
    iops=$(echo "$line" | sed -E 's/.*IOPS=([0-9.k]+).*/\1/')
    record fio "$name" "$(fiomib "$bw")" "$iops" "$rc"
}

fio --name=warmup-sw --directory="$DIR" --filename_format='f.$jobnum' \
    --rw=write --bs=1m --size="$FIO_SIZE" --numjobs="$FIO_JOBS" \
    --ioengine=psync --direct=1 --group_reporting >"$LOGDIR/warmup-sw.log" 2>&1
wrc=$?
wbw=$(grep -oE 'BW=[0-9.]+[KMG]iB/s' "$LOGDIR/warmup-sw.log" | head -1 | \
      sed -E 's/BW=([0-9.]+)([KMG])iB\/s/\1 \2/')
record fio warmup-sw "$(fiomib "$wbw")" "-" "$wrc"

SUITE=(
    "sw-1m write 1m $FIO_SIZE"
    "ow-1m write 1m $FIO_SIZE"
    "sr-1m read 1m $FIO_SIZE"
    "rw-1m randwrite 1m $FIO_SIZE"
    "rr-1m randread 1m $FIO_SIZE"
    "rw-128k randwrite 128k $FIO_SIZE"
    "rr-128k randread 128k $FIO_SIZE"
    "rw-4k randwrite 4k $FIO_SIZE4K"
    "rr-4k randread 4k $FIO_SIZE4K"
)
for job in "${SUITE[@]}"; do
    # shellcheck disable=SC2086
    run_fio $job
done

echo "PERF_NODE_DONE host=$HOST mode=$MODE $(date -Is)" >>"$OUT"
