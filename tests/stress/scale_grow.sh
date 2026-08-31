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

# Check EVERY host, and re-check before every step rather than once at startup.
#
# A dead efs-fuse leaves $MNT as an ordinary local directory, and the workers
# then happily create files on the node's own disk: the run reports a rate no
# cluster could produce and a per-inode size no metadata could fit in. One run
# spanning a cluster wipe recorded 500k creates/s at 10 B/inode against a
# measured ceiling of ~40k/s and ~1.3 KB/inode -- entirely local-disk numbers.
# A startup-only check on a single host cannot catch that, because these runs
# last hours and the mount goes away in the middle.
check_mounts() { # context
  local bad=""
  for h in $HOSTS; do
    EFS_SSH_TIMEOUT=20 $SSH "$h.ib" \
      "findmnt -n -o FSTYPE $MNT 2>/dev/null" | grep -q '^fuse\.efs-fuse$' \
      || bad="$bad $h"
  done
  [ -z "$bad" ] && return 0
  say "FAIL ($1): $MNT is not a live efs-fuse mount on:$bad"
  say "      refusing to continue -- results would be from the local disk"
  return 1
}

check_mounts startup || exit 2

# Post-mkfs catchup: Heal idle is not enough. A 9×16 create burst into the
# catchup window fences tables (ino_dup / CREATE EIO) and the probe then
# sees n=0. Wait until gens match AND rebuild counters stop moving.
wait_idle() { # context
  local seed="$PRIMARY" primary_host="fcstor003.ib"
  local i st idle gens ng prev="" cur
  say "wait idle ($1)"
  for i in $(seq 1 45); do
    st=$(EFS_SSH_TIMEOUT=10 $SSH "$primary_host" "cd /tmp/efs && ./efs-mgmt status $seed" 2>/dev/null) || true
    idle=$(printf '%s\n' "$st" | grep -c 'idle' || true)
    gens=$(printf '%s\n' "$st" | sed -n 's/.*gen=\([0-9][0-9]*\).*/\1/p' | sort -u)
    ng=$(printf '%s\n' "$gens" | grep -c . || true)
    if [ "$idle" -ge 4 ] && [ "$ng" = 1 ]; then
      break
    fi
    if [ "$i" -eq 45 ]; then
      say "FAIL: cluster not Heal-idle after 90s ($1)"
      return 1
    fi
    sleep 2
  done
  prev=$(count_rebuilds)
  sleep 3
  for i in $(seq 1 20); do
    cur=$(count_rebuilds)
    if [ "$cur" = "$prev" ]; then
      say "  idle gen=$(printf '%s' "$gens" | head -1) rebuilds=$cur"
      return 0
    fi
    prev=$cur
    sleep 3
  done
  say "FAIL: rebuilds still growing ($prev) ($1)"
  return 1
}

count_rebuilds() {
  local t=0 s n
  for s in $SERVERS; do
    n=$(EFS_SSH_TIMEOUT=10 $SSH "$s.ib" "grep -cE 'meta-rebuild:|raced with GC' /tmp/efs/efsd.log 2>/dev/null" || true)
    n=$(printf '%s' "$n" | tr -d '[:space:]')
    n=${n:-0}
    t=$((t + n))
  done
  echo "$t"
}

wait_idle startup || exit 2

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

  check_mounts "before step $target" || exit 2
  wait_idle "before $target" || exit 2

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
  missing=""
  for h in $HOSTS; do
    grep -q '^ERRORS ' "$OUTDIR/grow-$target-$h.txt" 2>/dev/null || missing="$missing $h"
  done
  if [ -n "$missing" ]; then
    say "FAIL: grow logs missing ERRORS (worker crashed) on:$missing"
    say "      refusing to continue -- rates would be wall-clock fiction"
    exit 2
  fi
  if [ "$nerr" -gt $((step_files / 20)) ]; then
    say "FAIL: errors=$nerr > 5% of $step_files — catchup/GC or ino_dup, not a scale curve"
    exit 2
  fi

  read -r rss_max rss_sum <<< "$(server_rss)"
  # RSS is what decides whether 2^32 is reachable on this hardware, so carry
  # the per-inode figure directly rather than making the reader divide.
  bpi=$(awk -v s="$rss_sum" -v n="$target" 'BEGIN {printf "%.0f", s * 1048576.0 / n}')

  say "  $step_files files in ${step_sec}s = $rate/s | errors=$nerr | rss max ${rss_max}MB sum ${rss_sum}MB = ${bpi}B/inode"
  printf '%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n' \
    "$target" "$per_client" "$step_files" "$step_sec" "$rate" "$nerr" "$rss_max" "$rss_sum" "$bpi" >> "$TSV"

  wait_idle "after $target" || exit 2
  say "  probing idle latency at $target"
  EFS_SSH_TIMEOUT=600 $SSH "$first.ib" \
    "python3 /tmp/efs/tests/stress/scale_probe.py $MNT $first $per_client $FANOUT 200" \
    > "$OUTDIR/probe-$target.txt" 2>&1
  sed 's/^/    /' "$OUTDIR/probe-$target.txt"
  if grep -q 'stat .* n=0' "$OUTDIR/probe-$target.txt" 2>/dev/null || \
     grep -q 'FileNotFoundError' "$OUTDIR/probe-$target.txt" 2>/dev/null; then
    say "FAIL: probe at $target saw no files (n=0) — tree missing or not FUSE"
    exit 2
  fi

  prev=$target
done

say "curve:"
column -t "$TSV" | sed 's/^/  /'
say "results in $OUTDIR"
