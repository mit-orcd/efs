#!/bin/bash
# ecopy_rounds.sh — copy ImageNet into efs from every client, in repeated rounds.
#
# ImageNet is the metadata-heavy shape efs is worst at: ~1300 files per class
# directory averaging ~117 KB, so throughput is decided by create/lookup round
# trips rather than bandwidth. A single client measured ~66 files/s both on
# Aug 24 and today, which is the number the metadata-op-storm work exists to
# move.
#
# Rounds matter as much as the rate. Each round hands every client a slice of
# class directories it has never copied, so the table only grows and a round is
# directly comparable to the one before it. If throughput sags across rounds,
# that is the table-growth cost (a grown chunk table has silently cost 15-40%
# before) rather than a cold-start artifact.
#
# Each client writes under its own subtree, so this measures aggregate
# throughput without same-directory contention. Point SHARED_DIR=1 at one
# directory to measure the opposite.
#
# Usage (login node, cluster up and all clients mounted):
#   tests/stress/ecopy_rounds.sh
#   ROUNDS=10 DIRS_PER_CLIENT=4 tests/stress/ecopy_rounds.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
HOSTS="${HOSTS:-fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015}"
SRC="${SRC:-/orcd/scratch/orcd/001/erbmi1/imagenet/images_complete/ilsvrc/train}"
ECOPY="${ECOPY:-$HOME/bin/ecopy}"
ROUNDS="${ROUNDS:-5}"
DIRS_PER_CLIENT="${DIRS_PER_CLIENT:-2}"
SHARED_DIR="${SHARED_DIR:-0}"
# A class dir is ~1300 files; at ~66 files/s that is ~20 s per dir per client.
ROUND_TIMEOUT="${ROUND_TIMEOUT:-900}"

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/ecopy/$STAMP}"
mkdir -p "$OUTDIR"
TSV="$OUTDIR/rounds.tsv"

nh=0
for _ in $HOSTS; do nh=$((nh + 1)); done

say() { echo "[ecopy] $*"; }

# The class list is the unit of work; slice it by index so no two clients and
# no two rounds ever touch the same directory.
mapfile -t CLASSES < <(ls "$SRC" 2>/dev/null | sort)
ncls=${#CLASSES[@]}
[ "$ncls" -gt 0 ] || { say "FAIL: no class dirs under $SRC"; exit 2; }

need=$((ROUNDS * nh * DIRS_PER_CLIENT))
if [ "$need" -gt "$ncls" ]; then
  say "FAIL: need $need class dirs but $SRC has $ncls"
  say "      lower ROUNDS or DIRS_PER_CLIENT"
  exit 2
fi

say "clients=$nh rounds=$ROUNDS dirs/client/round=$DIRS_PER_CLIENT"
say "source=$SRC ($ncls classes, using $need) -> $OUTDIR"
printf 'round\tfiles\tbytes\twall_sec\tfiles_per_sec\tmib_per_sec\tslowest_host\tfailed_hosts\n' > "$TSV"

# A dead mount turns this into a local-disk benchmark that looks great, so
# check every host before every round rather than trusting the last one.
check_mounts() { # context
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

  hi=0
  for h in $HOSTS; do
    # This client's slice for this round.
    base=$(( ((r - 1) * nh + hi) * DIRS_PER_CLIENT ))
    dirs=""
    for k in $(seq 0 $((DIRS_PER_CLIENT - 1))); do
      dirs="$dirs ${CLASSES[$((base + k))]}"
    done
    if [ "$SHARED_DIR" = "1" ]; then
      dst="$MNT/ecopy-shared"
    else
      # Per-host ROOT name (not a shared /ecopy parent). Concurrent mkdir of
      # the same ROOT dir returns ENOENT for the losers, and peer LOOKUP of
      # children under a hashed ROOT dest is not visible to mkdir -p.
      dst="$MNT/ecopy-$h/r$r"
    fi
    EFS_SSH_TIMEOUT="$ROUND_TIMEOUT" $SSH "$h.ib" "
        mkdir -p '$dst' || exit 1
        t0=\$(date +%s.%N)
        erc=0
        for c in $dirs; do
            # ecopy copies the source directory's CONTENTS into the target, so
            # give each class its own directory -- otherwise every class lands
            # flat in one dir, which is not ImageNet's shape.
            mkdir -p '$dst'/\$c || exit 1
            $ECOPY '$SRC'/\$c '$dst'/\$c >>/tmp/ecopy-round.log 2>&1 || erc=2
        done
        t1=\$(date +%s.%N)
        n=\$(find '$dst' -type f 2>/dev/null | wc -l)
        b=\$(du -sb '$dst' 2>/dev/null | cut -f1)
        echo \"RESULT files=\${n:-0} bytes=\${b:-0} secs=\$(echo \"\$t1-\$t0\" | bc) erc=\${erc}\"
    " > "$OUTDIR/r$r-$h.txt" 2>&1 &
    hi=$((hi + 1))
  done
  wall0=$(date +%s.%N)
  wait
  wall1=$(date +%s.%N)

  files=0; bytes=0; failed=""; slow_h="-"; slow_s=0
  for h in $HOSTS; do
    line=$(grep -h '^RESULT' "$OUTDIR/r$r-$h.txt" 2>/dev/null | tail -1)
    if [ -z "$line" ]; then failed="$failed $h"; continue; fi
    f=$(echo "$line" | sed 's/.*files=\([0-9]*\).*/\1/')
    b=$(echo "$line" | sed 's/.*bytes=\([0-9]*\).*/\1/')
    s=$(echo "$line" | sed 's/.*secs=\([0-9.]*\).*/\1/')
    erc=$(echo "$line" | sed -n 's/.*erc=\([0-9]*\).*/\1/p')
    [ "${erc:-0}" != "0" ] && failed="$failed $h"
    files=$((files + f)); bytes=$((bytes + b))
    if [ "$(echo "$s > $slow_s" | bc)" = "1" ]; then
      slow_s=$(printf '%.1f' "$s"); slow_h="$h"
    fi
  done
  wall=$(echo "$wall1 - $wall0" | bc)
  fps=$(echo "scale=1; $files / $wall" | bc 2>/dev/null)
  mbs=$(echo "scale=1; $bytes / 1048576 / $wall" | bc 2>/dev/null)
  say "round $r: files=$files wall=${wall}s ${fps} files/s ${mbs} MiB/s${failed:+ FAILED:$failed}"
  printf '%d\t%d\t%d\t%.1f\t%s\t%s\t%s\t%s\n' \
    "$r" "$files" "$bytes" "$wall" "$fps" "$mbs" \
    "$slow_h@${slow_s}s" "${failed:--}" >> "$TSV"
done

say "done -> $TSV"
column -t "$TSV"
