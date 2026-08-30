#!/bin/bash
# ecopy_verify.sh — prove the ecopy'd data is really on the cluster.
#
# Counting files from the client that wrote them proves very little: that client
# still has the tree in its dcache and can answer from memory. Two modes:
#
#   counts  (default) — per-class file counts vs the source. ImageNet class dirs
#                       hold 732-1300 images, so a round total below the nominal
#                       23400 is usually variance, not loss; only a per-class
#                       comparison separates the two.
#   deep              — read the bytes back from a DIFFERENT client than the one
#                       that wrote them, after remounting it, and compare md5
#                       against the source. This is the mode that answers
#                       "did it actually reach the cluster".
#
# Usage: tests/stress/ecopy_verify.sh [counts|deep] [host ...]
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs-mount}"
SRC="${SRC:-/orcd/scratch/orcd/001/erbmi1/imagenet/images_complete/ilsvrc/train}"
MODE="counts"
case "${1:-}" in counts|deep) MODE=$1; shift ;; esac
HOSTS="${*:-fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015}"
# Files per writer-host to byte-compare in deep mode.
DEEP_SAMPLE="${DEEP_SAMPLE:-20}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- counts -----------------------------------------------------------------
if [ "$MODE" = "counts" ]; then
  for h in $HOSTS; do
    EFS_SSH_TIMEOUT=600 $SSH "$h.ib" \
      "find '$MNT/ecopy/$h' -mindepth 2 -maxdepth 2 -type d 2>/dev/null | \
       while read -r d; do echo \"\$(basename \$d) \$(ls -U \"\$d\" | wc -l) \$d\"; done" \
      > "$tmp/$h.txt" 2>/dev/null &
  done
  wait

  bad=0; tot=0; sfiles=0; dfiles=0
  for h in $HOSTS; do
    while read -r cls n path; do
      [ -n "${cls:-}" ] || continue
      want=$(ls -U "$SRC/$cls" 2>/dev/null | wc -l)
      tot=$((tot + 1)); sfiles=$((sfiles + want)); dfiles=$((dfiles + n))
      if [ "$n" != "$want" ]; then
        echo "MISMATCH $path: have=$n want=$want (missing $((want - n)))"
        bad=$((bad + 1))
      fi
    done < "$tmp/$h.txt"
  done
  echo "---"
  echo "class dirs checked : $tot"
  echo "source files       : $sfiles"
  echo "efs files          : $dfiles"
  echo "mismatched dirs    : $bad"
  [ "$bad" -eq 0 ] && echo "VERIFY-OK" || echo "VERIFY-FAIL"
  exit $((bad > 0))
fi

# --- deep -------------------------------------------------------------------
# Rotate readers so no host ever checks its own writes, and remount the reader
# first so nothing can be served from a cache warmed during the copy.
set -- $HOSTS
nh=$#
readers=""
i=0
for h in $HOSTS; do
  j=$(((i + 1) % nh))
  k=0
  for r in $HOSTS; do [ "$k" = "$j" ] && readers="$readers $h:$r"; k=$((k + 1)); done
  i=$((i + 1))
done

echo "remounting readers so reads cannot come from a warm cache..."
for h in $HOSTS; do
  EFS_SSH_TIMEOUT=60 $SSH "$h.ib" "
      fusermount3 -u '$MNT' >/dev/null 2>&1
      for i in \$(seq 1 50); do pgrep -x efs-fuse >/dev/null || break; sleep 0.2; done
      pgrep -x efs-fuse >/dev/null && killall -9 efs-fuse >/dev/null 2>&1
      cd /tmp/efs && rm -f fuse.log
      setsid ./efs-fuse 172.16.223.57:19810 efs-test '$MNT' >fuse.log 2>&1 </dev/null &
      for i in \$(seq 1 100); do
          findmnt -n -o FSTYPE '$MNT' 2>/dev/null | grep -q fuse.efs-fuse && exit 0
          sleep 0.2
      done
      exit 1
  " >/dev/null 2>&1 &
done
wait

bad=0; checked=0
for pair in $readers; do
  writer=${pair%%:*}; reader=${pair##*:}
  # Sample files the writer produced, then read them back on the reader.
  EFS_SSH_TIMEOUT=300 $SSH "$reader.ib" \
    "find '$MNT/ecopy/$writer' -type f 2>/dev/null | head -$DEEP_SAMPLE | \
     while read -r f; do echo \"\$(md5sum \"\$f\" | cut -d' ' -f1) \$f\"; done" \
    > "$tmp/deep-$writer.txt" 2>/dev/null
  while read -r sum path; do
    [ -n "${sum:-}" ] || continue
    # .../ecopy/<writer>/r<N>/<class>/<file> -> source is <class>/<file>
    cls=$(basename "$(dirname "$path")"); f=$(basename "$path")
    want=$(md5sum "$SRC/$cls/$f" 2>/dev/null | cut -d' ' -f1)
    checked=$((checked + 1))
    if [ -z "$want" ]; then
      echo "NO-SOURCE $path"; bad=$((bad + 1))
    elif [ "$sum" != "$want" ]; then
      echo "BAD-BYTES $path (read on $reader): got=$sum want=$want"
      bad=$((bad + 1))
    fi
  done < "$tmp/deep-$writer.txt"
  echo "  $writer's files read on $reader: $(wc -l < "$tmp/deep-$writer.txt") checked"
done

echo "---"
echo "files byte-compared : $checked (cross-client, after remount)"
echo "bad                 : $bad"
[ "$bad" -eq 0 ] && [ "$checked" -gt 0 ] && echo "VERIFY-OK" || echo "VERIFY-FAIL"
[ "$bad" -eq 0 ] && [ "$checked" -gt 0 ]
