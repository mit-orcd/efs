#!/bin/bash
# fio_rounds.sh — rounds of fio from every client, on a table that keeps growing.
#
# The ecopy rounds showed metadata throughput decaying 2.2x as the table grew.
# This asks the same question of the data path: every round writes a fresh
# directory per client, so round N runs against a strictly larger table than
# round N-1 and the two numbers are directly comparable. A flat curve here with
# a decaying ecopy curve says the cost is metadata-side; a decaying curve here
# says it is in the flush/report path that both share.
#
# Each client writes its own directory, so this is aggregate throughput without
# same-directory contention.
#
# Usage (login node, cluster up and all clients mounted):
#   tests/stress/fio_rounds.sh
#   ROUNDS=8 JOB=rw-4k tests/stress/fio_rounds.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
HOSTS="${HOSTS:-fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015}"
ROUNDS="${ROUNDS:-6}"
FIO_JOBS="${FIO_JOBS:-9}"
FIO_SIZE="${FIO_SIZE:-1g}"
FIO_RUNTIME="${FIO_RUNTIME:-30}"
# name rw bs — one workload per round, repeated. sw-1m is the streaming write
# ceiling; rw-4k is the read-modify-write path that chunk metadata dominates.
JOB="${JOB:-sw-1m}"
ROUND_TIMEOUT="${ROUND_TIMEOUT:-600}"

case "$JOB" in
  sw-1m)   RW=write     BS=1m   ;;
  ow-1m)   RW=write     BS=1m   ;;
  sr-1m)   RW=read      BS=1m   ;;
  rw-4k)   RW=randwrite BS=4k   ;;
  rr-4k)   RW=randread  BS=4k   ;;
  rw-128k) RW=randwrite BS=128k ;;
  *) echo "unknown JOB=$JOB"; exit 2 ;;
esac

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/fio-rounds/$STAMP}"
mkdir -p "$OUTDIR"
TSV="$OUTDIR/rounds.tsv"

nh=0
for _ in $HOSTS; do nh=$((nh + 1)); done
say() { echo "[fio] $*"; }

say "clients=$nh rounds=$ROUNDS job=$JOB ($RW bs=$BS) jobs/client=$FIO_JOBS"
say "-> $OUTDIR"
printf 'round\tagg_mib_s\tagg_iops\tslowest_host\tfailed_hosts\n' > "$TSV"

# A dead mount silently turns this into a local-disk benchmark, so re-check
# every host before every round rather than trusting the last one.
check_mounts() {
  local bad=""
  for h in $HOSTS; do
    EFS_SSH_TIMEOUT=20 $SSH "$h.ib" \
      "findmnt -n -o FSTYPE $MNT 2>/dev/null" | grep -q '^fuse\.efs-fuse$' \
      || bad="$bad $h"
  done
  [ -z "$bad" ] && return 0
  say "FAIL ($1): $MNT is not a live efs-fuse mount on:$bad"
  return 1
}

check_mounts startup || exit 2

for r in $(seq 1 "$ROUNDS"); do
  say "--- round $r/$ROUNDS ---"
  check_mounts "before round $r" || exit 2

  for h in $HOSTS; do
    dir="$MNT/fio/$h/r$r"
    EFS_SSH_TIMEOUT="$ROUND_TIMEOUT" $SSH "$h.ib" "
        mkdir -p '$dir' || exit 1
        fio --name=$JOB --directory='$dir' --filename_format='f.\$jobnum' \
            --rw=$RW --bs=$BS --size=$FIO_SIZE --numjobs=$FIO_JOBS \
            --ioengine=psync --direct=1 --time_based \
            --runtime=$FIO_RUNTIME --ramp_time=2 --group_reporting \
            --randrepeat=0 2>&1
    " > "$OUTDIR/r$r-$h.log" 2>&1 &
  done
  wait

  agg_mib=0; agg_iops=0; failed=""; slow_h="-"; slow_mib=""
  for h in $HOSTS; do
    line=$(grep -E '^\s+(read|write): IOPS=' "$OUTDIR/r$r-$h.log" | head -1)
    if [ -z "$line" ]; then failed="$failed $h"; continue; fi
    # BW=NNN{K,M,G}iB/s -> MiB/s
    bw=$(echo "$line" | sed -E 's#.*BW=([0-9.]+)([KMG])iB/s.*#\1 \2#')
    v=$(echo "$bw" | awk '{print $1}'); u=$(echo "$bw" | awk '{print $2}')
    case "$u" in
      K) mib=$(echo "scale=1; $v/1024" | bc) ;;
      G) mib=$(echo "scale=1; $v*1024" | bc) ;;
      *) mib=$v ;;
    esac
    io=$(echo "$line" | sed -E 's/.*IOPS=([0-9.]+)(k?).*/\1 \2/')
    iv=$(echo "$io" | awk '{print $1}'); iu=$(echo "$io" | awk '{print $2}')
    [ "$iu" = "k" ] && iv=$(echo "$iv * 1000" | bc)
    agg_mib=$(echo "$agg_mib + $mib" | bc)
    agg_iops=$(echo "$agg_iops + $iv" | bc)
    if [ -z "$slow_mib" ] || [ "$(echo "$mib < $slow_mib" | bc)" = "1" ]; then
      slow_mib=$mib; slow_h="$h"
    fi
  done
  say "round $r: ${agg_mib} MiB/s agg, ${agg_iops} IOPS agg${failed:+ FAILED:$failed}"
  printf '%d\t%s\t%s\t%s\t%s\n' "$r" "$agg_mib" "$agg_iops" \
    "$slow_h@${slow_mib:-?}MiB/s" "${failed:--}" >> "$TSV"
done

say "done -> $TSV"
column -t "$TSV"
