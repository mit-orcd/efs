#!/bin/bash
# fio_rounds.sh — rounds of honest fio from every client, table keeps growing.
#
# The ecopy rounds showed metadata throughput decaying 2.2x as the table grew.
# This asks the same question of the data path: every round writes a fresh
# directory per client, so round N runs against a strictly larger table than
# round N-1. A flat curve here with a decaying ecopy curve says the cost is
# metadata-side; a decaying curve here says it is in the flush/report path
# that both share.
#
# Honest method (see .cursor/rules/efs-fio-honest.mdc): writes are
# --end_fsync=1 and NOT time_based (flush is in the clock). Reads remount
# first. findmnt must be fuse.efs-fuse; NOT_FUSE or Disk stats: md0/sda abort.
# Shape: 9 jobs × 2g (4k jobs 256m), psync, direct=1.
#
# Usage (login node, cluster up and all clients mounted, auto RDMA):
#   tests/stress/fio_rounds.sh
#   ROUNDS=5 JOB=rw-4k tests/stress/fio_rounds.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
HOSTS="${HOSTS:-fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015}"
ROUNDS="${ROUNDS:-6}"
FIO_JOBS="${FIO_JOBS:-9}"
FIO_SIZE="${FIO_SIZE:-2g}"
FIO_SIZE4K="${FIO_SIZE4K:-256m}"
JOB="${JOB:-sw-1m}"
ROUND_TIMEOUT="${ROUND_TIMEOUT:-400}"

case "$JOB" in
  sw-1m)   RW=write     BS=1m   KIND=write SIZE="$FIO_SIZE"   ;;
  ow-1m)   RW=write     BS=1m   KIND=write SIZE="$FIO_SIZE"   ;;
  rw-1m)   RW=randwrite BS=1m   KIND=write SIZE="$FIO_SIZE"   ;;
  sr-1m)   RW=read      BS=1m   KIND=read  SIZE="$FIO_SIZE"   ;;
  rw-4k)   RW=randwrite BS=4k   KIND=write SIZE="$FIO_SIZE4K" ;;
  rr-4k)   RW=randread  BS=4k   KIND=read  SIZE="$FIO_SIZE4K" ;;
  rw-128k) RW=randwrite BS=128k KIND=write SIZE="$FIO_SIZE"   ;;
  rr-128k) RW=randread  BS=128k KIND=read  SIZE="$FIO_SIZE"   ;;
  *) echo "unknown JOB=$JOB"; exit 2 ;;
esac

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/fio-rounds/$STAMP}"
mkdir -p "$OUTDIR"
TSV="$OUTDIR/rounds.tsv"

nh=0
for _ in $HOSTS; do nh=$((nh + 1)); done
say() { echo "[fio] $*"; }

say "clients=$nh rounds=$ROUNDS job=$JOB ($RW bs=$BS size=$SIZE) jobs/client=$FIO_JOBS"
say "honest: writes --end_fsync=1, no time_based; reads remount first"
say "-> $OUTDIR"
printf 'round\tagg_mib_s\tagg_iops\tslowest_host\tfailed_hosts\n' > "$TSV"

check_mounts() {
  local bad="" h fst
  for h in $HOSTS; do
    fst=$(EFS_SSH_TIMEOUT=10 $SSH "$h.ib" \
      "findmnt -n -o FSTYPE $MNT 2>/dev/null" 2>/dev/null | tr -d '\r')
    echo "$fst" | grep -q '^fuse\.efs-fuse$' || bad="$bad $h:$fst"
  done
  [ -z "$bad" ] && return 0
  say "FAIL ($1): $MNT is not a live efs-fuse mount on:$bad"
  return 1
}

# Reads of a same-mount write are dcache. Kill+remount every participating
# client (same as fio_honest_matrix.sh). Empty EFS_TRANSPORT = auto RDMA.
remount_all() {
  local h pids=() p rc=0
  say "remount ${HOSTS}"
  for h in $HOSTS; do
    EFS_SSH_TIMEOUT=25 $SSH "$h.ib" "killall -9 efs-fuse 2>/dev/null || true
        timeout 3 fusermount3 -uz $MNT 2>/dev/null || true
        cd /tmp/efs && mkdir -p $MNT && rm -f fuse.log
        EFS_TRANSPORT='${EFS_TRANSPORT:-}' setsid ./efs-fuse 172.16.223.57:19810 efs-test $MNT >fuse.log 2>&1 </dev/null &
        for i in \$(seq 1 40); do
            grep -q \"efs-fuse $MNT \" /proc/mounts || continue
            timeout 1 stat $MNT >/dev/null 2>&1 && exit 0
            sleep 0.15
        done
        echo remount-fail; tail -5 fuse.log; exit 1" >/dev/null &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || rc=1; done
  return $rc
}

parse_bw_line() {
  local line=$1
  local bw v u mib io iv iu
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
  echo "$mib $iv"
}

# Unique ROOT name per host so 9 clients do not race mkdir of the same
# hashed directory (concurrent CREATE of one name has wedged RDMA QPs).
dir_for() { # host round
  echo "$MNT/fio-$1/r$2"
}

check_mounts startup || exit 2
# Fresh QPs: a previous raced mkdir can leave the mount serving stat but
# EIO/hang on the next CREATE.
remount_all || { say "FAIL startup remount"; exit 2; }
check_mounts "after startup remount" || exit 2

for r in $(seq 1 "$ROUNDS"); do
  say "--- round $r/$ROUNDS ---"
  check_mounts "before round $r" || exit 2
  if [ "$KIND" = read ]; then
    remount_all || { say "FAIL remount before read round $r"; exit 2; }
    check_mounts "after remount round $r" || exit 2
  fi

  extra=""
  [ "$KIND" = write ] && extra="--end_fsync=1"

  for h in $HOSTS; do
    dir=$(dir_for "$h" "$r")
    EFS_SSH_TIMEOUT="$ROUND_TIMEOUT" $SSH "$h.ib" "
        fst=\$(findmnt -n -o FSTYPE '$MNT' 2>/dev/null || true)
        echo FUSE_CHECK fstype=\$fst mnt=$MNT
        echo \$fst | grep -q '^fuse\\.efs-fuse\$' || { echo NOT_FUSE; exit 1; }
        echo FUSE_OK
        mkdir -p '$dir' || exit 1
        fio --name=$JOB --directory='$dir' --filename_format='f.\$jobnum' \
            --rw=$RW --bs=$BS --size=$SIZE --numjobs=$FIO_JOBS \
            --ioengine=psync --direct=1 --group_reporting \
            $extra --randrepeat=0 2>&1
    " > "$OUTDIR/r$r-$h.log" 2>&1 &
  done
  wait

  agg_mib=0; agg_iops=0; failed=""; slow_h="-"; slow_mib=""
  for h in $HOSTS; do
    log="$OUTDIR/r$r-$h.log"
    if grep -q NOT_FUSE "$log" 2>/dev/null || \
       grep -qE 'Disk stats:.*md0|Disk stats:.*sda' "$log" 2>/dev/null; then
      say "ABORT $h round $r: NOT_FUSE or local-disk stats"
      failed="$failed $h"
      continue
    fi
    grep -q FUSE_OK "$log" 2>/dev/null || { failed="$failed $h"; continue; }
    line=$(grep -E '^\s+(read|write): IOPS=' "$log" | head -1)
    if [ -z "$line" ]; then failed="$failed $h"; continue; fi
    parsed=$(parse_bw_line "$line")
    mib=$(echo "$parsed" | awk '{print $1}')
    iv=$(echo "$parsed" | awk '{print $2}')
    agg_mib=$(echo "$agg_mib + $mib" | bc)
    agg_iops=$(echo "$agg_iops + $iv" | bc)
    if [ -z "$slow_mib" ] || [ "$(echo "$mib < $slow_mib" | bc)" = 1 ]; then
      slow_mib=$mib; slow_h="$h"
    fi
  done
  say "round $r: ${agg_mib} MiB/s agg, ${agg_iops} IOPS agg${failed:+ FAILED:$failed}"
  printf '%d\t%s\t%s\t%s\t%s\n' "$r" "$agg_mib" "$agg_iops" \
    "$slow_h@${slow_mib:-?}MiB/s" "${failed:--}" >> "$TSV"
  if [ -n "$failed" ]; then
    say "FAIL round $r had failed hosts:$failed"
    exit 1
  fi
done

say "done -> $TSV"
column -t "$TSV"
