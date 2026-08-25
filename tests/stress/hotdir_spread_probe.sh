#!/bin/bash
# hotdir_spread_probe.sh — Phase 3b Item 2 gate.
#
# Creates N files in one directory on a bits=3 export (N default 300000 so
# the dir crosses EFS_DIR_SPREAD_MIN=65536), checks readdir count, peer
# visibility without remount, a handful of renames, then rmdir after empty.
#
# Usage (cluster already up, efs-s3 bits=3 mounted on A and B):
#   tests/stress/hotdir_spread_probe.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
CLIENT_A="${CLIENT_A:-fcstor007.ib}"
CLIENT_B="${CLIENT_B:-fcstor008.ib}"
N="${N:-300000}"
MNT="${MNT:-/tmp/efs/mnt}"
DIR=hotdir-spread
FAIL=0

say() { echo "[hotdir] $*"; }
bad() { echo "[hotdir] FAIL: $*"; FAIL=$((FAIL+1)); }

say "create $N files in $MNT/$DIR on $CLIENT_A"
$SSH "$CLIENT_A" "python3 - <<'PY'
import os, sys
mnt = os.environ.get('MNT', '$MNT')
n = int(os.environ.get('N', '$N'))
d = os.path.join(mnt, '$DIR')
os.makedirs(d, exist_ok=True)
for i in range(n):
    p = os.path.join(d, 'f%06d' % i)
    fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o644)
    os.close(fd)
    if i and i % 50000 == 0:
        print('created', i, flush=True)
print('created', n, flush=True)
PY
"

say "readdir count on A"
A_N=$($SSH "$CLIENT_A" "python3 -c \"import os; print(len(os.listdir('$MNT/$DIR')))\"")
say "A saw $A_N"
[ "$A_N" = "$N" ] || bad "A readdir $A_N != $N"

say "peer readdir on B (no remount)"
B_N=$($SSH "$CLIENT_B" "python3 -c \"import os; print(len(os.listdir('$MNT/$DIR')))\"")
say "B saw $B_N"
[ "$B_N" = "$N" ] || bad "B readdir $B_N != $N"

say "rename churn 100 names"
$SSH "$CLIENT_A" "
cd $MNT/$DIR
for i in \$(seq 0 99); do
    mv f\$(printf '%06d' \$i) r\$(printf '%06d' \$i) || exit 1
done
for i in \$(seq 0 99); do
    mv r\$(printf '%06d' \$i) f\$(printf '%06d' \$i) || exit 1
done
echo rename-ok
"

say "empty + rmdir"
$SSH "$CLIENT_A" "python3 - <<'PY'
import os, shutil
d = '$MNT/$DIR'
for name in os.listdir(d):
    os.unlink(os.path.join(d, name))
os.rmdir(d)
print('rmdir-ok')
PY
"

if [ "$FAIL" -eq 0 ]; then
    say "PASS"
    exit 0
fi
say "FAIL count=$FAIL"
exit 1
