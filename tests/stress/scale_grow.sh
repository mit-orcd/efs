#!/bin/bash
# scale_grow.sh — grow one export to millions of inodes, measuring on the way.
#
# Every performance number this project has is from a table under ~440k inodes,
# against a stated goal of 2^32. This walks the population up in steps and
# measures the same things at each step, so the shape of the curve tells us what
# breaks first instead of us extrapolating four orders of magnitude.
#
# At each step it records: create throughput for that step, idle per-op latency
# (stat/create/unlink/readdir), server RSS, and reported usage. RSS is the one
# that decides whether 2^32 is reachable at all on this hardware.
#
# Usage (login node, cluster up and mounted):
#   tests/stress/scale_grow.sh
#   STEPS="100000 500000 1000000" tests/stress/scale_grow.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
HOSTS="${HOSTS:-fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015}"
SERVERS="${SERVERS:-fcstor003 fcstor004 fcstor005 fcstor006}"
PRIMARY="${PRIMARY:-172.16.223.57:19810}"
THREADS="${THREADS:-16}"
FANOUT="${FANOUT:-1000}"
# Cumulative totals across ALL clients.
STEPS="${STEPS:-100000 500000 1000000 2000000 5000000 10000000}"

HERE=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/scale/$STAMP}"
mkdir -p "$OUTDIR"
TSV="$OUTDIR/curve.tsv"

nh=0
for _ in $HOSTS; do nh=$((nh + 1)); done
first=$(echo $HOSTS | awk '{print $1}')

say() { echo "[scale] $*"; }
say "clients=$nh threads=$THREADS fanout=$FANOUT -> $OUTDIR"
printf 'total\tper_client\tstep_files\tstep_sec\tcreates_per_sec\terrors\trss_mb_max\trss_mb_sum\tbytes_per_inode\n' > "$TSV"

# Push the workers next to the node-local build.
for h in $HOSTS; do
  EFS_SSH_TIMEOUT=20 $SSH "$h.ib" "mkdir -p /tmp/efs/tests/stress && cat > /tmp/efs/tests/stress/scale_worker.py" < "$HERE/scale_worker.py" &
done
wait
for h in $HOSTS; do
  EFS_SSH_TIMEOUT=20 $SSH "$h.ib" "cat > /tmp/efs/tests/stress/scale_probe.py" < "$HERE/scale_probe.py" &
done
wait

EFS_SSH_TIMEOUT=20 $SSH "$first.ib" "test -d $MNT && findmnt -n -o FSTYPE $MNT" | grep -q fuse || {
  say "FAIL: $MNT on $first is not a FUSE mount"
  exit 2
}

server_rss() {
  local sum=0 max=0
  for s in $SERVERS; do
    local kb
    kb=$(EFS_SSH_TIMEOUT=15 $SSH "$s.ib" "ps -o rss= -C efsd 2>/dev/null | tr -d ' ' | head -1")
    kb=${kb:-0}
    sum=$((sum + kb))
    [ "$kb" -gt "$max" ] && max=$kb
  done
  echo "$((max / 1024)) $((sum / 1024))"
}

prev=0
for target in $STEPS; do
  per_client=$((target / nh))
  prev_per=$((prev / nh))
  delta=$((per_client - prev_per))
  [ "$delta" -le 0 ] && continue

  say "growing to $target total ($per_client/client, +$delta each)"
  t0=$(date +%s)
  i=0
  for h in $HOSTS; do
    EFS_SSH_TIMEOUT=100000 $SSH "$h.ib" \
      "PYTHONUNBUFFERED=1 python3 /tmp/efs/tests/stress/scale_worker.py $MNT $h $prev_per $delta $THREADS $FANOUT" \
      > "$OUTDIR/grow-$target-$h.txt" 2>&1 &
    eval "gp_$i=$!"
    i=$((i + 1))
  done
  i=0
  for h in $HOSTS; do
    eval "wait \$gp_$i"
    i=$((i + 1))
  done
  t1=$(date +%s)
  step_sec=$((t1 - t0))
  [ "$step_sec" -le 0 ] && step_sec=1
  step_files=$((delta * nh))
  rate=$((step_files / step_sec))
  nerr=$(cat "$OUTDIR"/grow-$target-*.txt 2>/dev/null | awk '/^ERRORS/ {s += $2} END {print s + 0}')

  read -r rss_max rss_sum <<< "$(server_rss)"
  # RSS is what decides whether 2^32 is reachable on this hardware, so carry
  # the per-inode figure directly rather than making the reader divide.
  bpi=$(awk -v s="$rss_sum" -v n="$target" 'BEGIN {printf "%.0f", s * 1048576.0 / n}')

  say "  $step_files files in ${step_sec}s = $rate/s | errors=$nerr | rss max ${rss_max}MB sum ${rss_sum}MB = ${bpi}B/inode"
  printf '%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n' \
    "$target" "$per_client" "$step_files" "$step_sec" "$rate" "$nerr" "$rss_max" "$rss_sum" "$bpi" >> "$TSV"

  say "  probing idle latency at $target"
  EFS_SSH_TIMEOUT=600 $SSH "$first.ib" \
    "python3 /tmp/efs/tests/stress/scale_probe.py $MNT $first $per_client $FANOUT 200" \
    > "$OUTDIR/probe-$target.txt" 2>&1
  sed 's/^/    /' "$OUTDIR/probe-$target.txt"

  prev=$target
done

say "curve:"
column -t "$TSV" | sed 's/^/  /'
say "results in $OUTDIR"
