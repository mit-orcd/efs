#!/bin/bash
# unlink_storm.sh — N clients create thousands of files, then rmtree together.
#
# Reproduces the 9-way POSIX teardown hang: shutil.rmtree of a large FUSE
# tree from many clients at once, FUSE workers D-state in unlink/rmdir.
# Not a posix_suite test (not an XFS-compare).
#
# Usage (login node, cluster already up, FUSE at /tmp/efs/mnt):
#   tests/stress/unlink_storm.sh
#   NFILES=4000 tests/stress/unlink_storm.sh
#
# Pass: every client finishes rmtree within RM_SEC and the mount still
# accepts a create. Fail: any client times out (hang) or EIO.
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT="${MNT:-/tmp/efs/mnt}"
NFILES="${NFILES:-4000}"
NDIRS="${NDIRS:-20}"
RM_SEC="${RM_SEC:-400}"
WAIT_SEC="${WAIT_SEC:-600}"
HOSTS="${HOSTS:-fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib}"
HERE=$(cd "$(dirname "$0")" && pwd)
WORKER="$HERE/unlink_storm_worker.py"

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUTDIR="${OUTDIR:-$HOME/git/efs/results/stress/unlink-storm-$STAMP}"
mkdir -p "$OUTDIR"

say() { echo "[unlink-storm] $*"; }
ncli=0
for _ in $HOSTS; do ncli=$((ncli + 1)); done
say "clients=$ncli nfiles=$NFILES ndirs=$NDIRS rm_sec=$RM_SEC -> $OUTDIR"

first=
for h in $HOSTS; do first=$h; break; done
$SSH "$first" "test -d $MNT && echo MNT_OK" | grep -q MNT_OK || {
  say "FAIL: mount not usable on $first"
  exit 2
}
$SSH "$first" "python3 - <<PY
import os
m = '$MNT'
for n in os.listdir(m):
    if n.startswith('ustorm-ready-') or n.startswith('ustorm-alive-') or n == 'ustorm-go':
        try:
            os.unlink(os.path.join(m, n))
        except OSError:
            pass
print('cleared')
PY"

# Push worker next to the node-local suite (NFS home also works; this is faster).
for h in $HOSTS; do
  $SSH "$h" "mkdir -p /tmp/efs/tests/stress && cp -f '$WORKER' /tmp/efs/tests/stress/unlink_storm_worker.py" &
done
wait

i=0
for h in $HOSTS; do
  short=${h%.ib}
  $SSH "$h" "PYTHONUNBUFFERED=1 python3 /tmp/efs/tests/stress/unlink_storm_worker.py $MNT $NFILES $NDIRS $RM_SEC $WAIT_SEC" \
    > "$OUTDIR/log-$short.txt" 2>&1 &
  eval "wpid_$i=$!"
  i=$((i + 1))
done

say "waiting for $ncli ready files"
ready_deadline=$(( $(date +%s) + WAIT_SEC ))
while :; do
  hostlist=$(echo $HOSTS | sed 's/\.ib//g')
  nready=$($SSH "$first" "python3 - <<PY
import os
m='$MNT'
hosts='$hostlist'
print(sum(1 for h in hosts.split() if os.path.exists(os.path.join(m, 'ustorm-ready-' + h))))
PY" 2>/dev/null | tail -1 | tr -dc '0-9')
  nready=${nready:-0}
  if [ "$nready" -ge "$ncli" ]; then
    say "all $nready ready — go"
    break
  fi
  if [ "$(date +%s)" -ge "$ready_deadline" ]; then
    say "FAIL: only $nready/$ncli ready before go"
    break
  fi
  sleep 0.2
done

$SSH "$first" "touch $MNT/ustorm-go" || say "WARN: could not write ustorm-go"

fail=0
i=0
for h in $HOSTS; do
  short=${h%.ib}
  eval "wait \$wpid_$i"
  rc=$?
  line=$(grep -E '^(CREATE_OK|RM_OK|RM_HANG|RM_ERR|TIMEOUT_GO|ALIVE)' "$OUTDIR/log-$short.txt" 2>/dev/null | tr '\n' ' ')
  if grep -q '^RM_OK' "$OUTDIR/log-$short.txt" && grep -q '^ALIVE' "$OUTDIR/log-$short.txt"; then
    say "PASS $short  $line"
  else
    say "FAIL $short rc=$rc  $line"
    fail=$((fail + 1))
  fi
  i=$((i + 1))
done

if [ "$fail" -eq 0 ]; then
  say "PASS $ncli clients × $NFILES files parallel rmtree"
  echo PASS > "$OUTDIR/result.txt"
  exit 0
fi
say "FAIL $fail/$ncli clients hung or errored — see $OUTDIR"
echo FAIL > "$OUTDIR/result.txt"
exit 1
