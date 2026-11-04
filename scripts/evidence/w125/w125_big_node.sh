#!/bin/bash
# w125_big_node.sh ROLE MNT OUT DEADLINE_EPOCH [ARGS...]
#
# Node-side roles of the W125 big mixed test (started in named screen
# sessions by orcd_mini_big.sh; each runs until DEADLINE_EPOCH and logs to
# OUT/<role>.log):
#   dd      dd copies of local random sources (256 MiB, 1 GiB, 4 GiB) into
#           MNT/dd-<host> with bs 64k/1M/4M and conv=fsync, md5-compared with
#           the source; the last 6 copies are kept, older ones deleted.
#   compact efs-mgmt pack-compact SEED (ARGS: seed, efs-mgmt path) every 60 s;
#           every 5 min also efs-mgmt status.
#   efs     .efs commands every 120 s: stats --json, find report --json.
#   tiny    tiny_files.py MNT/tiny (ARGS: testing dir): up to 100,000 files,
#           1 B - 4 MiB, 16 workers.
#   md5     tiny_files_md5.py over MNT/tiny every pass (ARGS: testing dir,
#           compare script); each manifest is compared with the previous one
#           (files in both must hash the same, no read errors).
#   del     random_delete.py MNT/tiny (ARGS: testing dir), 30% of the files
#           found, every 600 s.
# A line per operation; "FAIL" marks a functional failure.
set -u
ROLE=$1; MNT=$2; OUT=$3; END=$4; shift 4
mkdir -p "$OUT"; L=$OUT/$ROLE.log; H=$(hostname -s)
now() { date -u +%H:%M:%S; }
case $ROLE in
dd)
  SRC=$OUT/src; mkdir -p "$SRC" "$MNT/dd-$H"
  for s in 256 1024 4096; do [ -f $SRC/r$s ] || head -c $((s << 20)) /dev/urandom > $SRC/r$s; md5sum $SRC/r$s | cut -c1-32 > $SRC/r$s.md5; done
  i=0
  while [ "$(date +%s)" -lt "$END" ]; do
    s=$(echo 256 1024 4096 | tr ' ' '\n' | shuf -n1); bs=$(echo 64k 1M 4M | tr ' ' '\n' | shuf -n1)
    f=$MNT/dd-$H/f$(printf %05d $i); t0=$(date +%s.%N)
    if dd if=$SRC/r$s of=$f bs=$bs conv=fsync status=none 2>> $L; then
      t1=$(date +%s.%N); m=$(md5sum < $f | cut -c1-32); t2=$(date +%s.%N)
      if [ "$m" = "$(cat $SRC/r$s.md5)" ]; then r=ok; else r="FAIL md5 $m"; fi
      echo "$(now) dd $i size=${s}M bs=$bs write_MBps=$(echo "$s / ($t1 - $t0)" | bc) read_MBps=$(echo "$s / ($t2 - $t1)" | bc) $r" >> $L
    else
      echo "$(now) dd $i size=${s}M bs=$bs FAIL dd rc=$?" >> $L
    fi
    [ $i -ge 6 ] && rm -f $MNT/dd-$H/f$(printf %05d $((i - 6)))
    i=$((i + 1))
  done ;;
compact)
  SEED=$1; MGMT=$2; n=0
  while [ "$(date +%s)" -lt "$END" ]; do
    echo "$(now) $($MGMT pack-compact $SEED 64 2>&1 | grep -o 'node [0-9].*leads group.*' | tr '\n' ' ')" >> $L
    [ $((n % 5)) -eq 0 ] && $MGMT status $SEED 2>&1 | grep -E 'node [0-9]:|Usable \(|Cluster state' | sed "s/^/$(now) /" >> $L
    n=$((n + 1)); sleep 60
  done ;;
efs)
  while [ "$(date +%s)" -lt "$END" ]; do
    t0=$(date +%s.%N); python3 $MNT/.efs/stats --json > /dev/null 2>> $L; r1=$?; t1=$(date +%s.%N)
    python3 $MNT/.efs/find report --json > /dev/null 2>> $L; r2=$?; t2=$(date +%s.%N)
    # exit 75 = "not indexed yet" (catalog refresh pending), not a failure
    s1=$([ $r1 = 0 ] || [ $r1 = 75 ] && echo ok || echo "FAIL rc=$r1"); s2=$([ $r2 = 0 ] || [ $r2 = 75 ] && echo ok || echo "FAIL rc=$r2")
    echo "$(now) stats $s1 $(echo "$t1 - $t0" | bc)s find $s2 $(echo "$t2 - $t1" | bc)s" >> $L
    sleep 120
  done ;;
tiny)
  T=$1
  timeout $((END - $(date +%s))) python3 $T/tiny_files.py $MNT/tiny --max-files 100000 --max-file-size 4194304 \
    --max-depth 3 --max-files-per-folder 100 --workers 16 >> $L 2>&1
  echo "$(now) tiny rc=$?" >> $L ;;
md5)
  T=$1; CMP=$2; k=0
  while [ "$(date +%s)" -lt "$END" ]; do
    [ -d $MNT/tiny ] || { sleep 30; continue; }
    python3 $T/tiny_files_md5.py $MNT/tiny --workers 16 --output $OUT/m$k.jsonl > $OUT/m$k.out 2>&1
    if [ $k -gt 0 ]; then echo "$(now) pass $k $(python3 $CMP $OUT/m$((k - 1)).jsonl $OUT/m$k.jsonl | head -n 1)" >> $L; else echo "$(now) pass 0 $(grep -c . $OUT/m0.jsonl) entries" >> $L; fi
    k=$((k + 1))
  done ;;
del)
  T=$1
  while [ "$(date +%s)" -lt "$END" ]; do
    sleep 600; [ -d $MNT/tiny ] || continue
    n=$(find $MNT/tiny -type f | wc -l)
    echo "$(now) delete $((n * 3 / 10)) of $n: $(python3 $T/random_delete.py $MNT/tiny -n $((n * 3 / 10)) 2>&1 | tail -n 1)" >> $L
  done ;;
*) echo "unknown role $ROLE"; exit 2 ;;
esac
