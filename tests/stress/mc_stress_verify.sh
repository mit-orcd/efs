#!/bin/bash
# mc_stress_verify.sh <mount> <iters>  — run once both workers finished.
set -u
M="$1"
N="${2:-100}"
FAIL=0
bad() { echo "FAIL: $*"; FAIL=$((FAIL+1)); }

# 1. cdir must hold 2N files, each with its own name as content
C=$(ls "$M/cdir" | wc -l)
[ "$C" -eq $((2 * N)) ] || bad "cdir count $C != $((2 * N))"
for f in "$M/cdir"/*; do
    want=$(basename "$f")
    got=$(cat "$f" 2>/dev/null)
    [ "$got" = "$want" ] || bad "cdir/$want content '$got'"
done

# 2. appfile: exactly 2N lines, each 'A-<i>' or 'B-<i>', no torn lines
L=$(wc -l < "$M/appfile" 2>/dev/null || echo 0)
[ "$L" -eq $((2 * N)) ] || bad "appfile lines $L != $((2 * N))"
BADLINES=$(grep -cvE '^[AB]-[0-9]+$' "$M/appfile" 2>/dev/null)
[ "${BADLINES:-0}" -eq 0 ] || bad "appfile has $BADLINES torn lines"
for T in A B; do
    CNT=$(grep -cE "^$T-" "$M/appfile" 2>/dev/null)
    [ "${CNT:-0}" -eq "$N" ] || bad "appfile has $CNT lines from $T, want $N"
done

# 3. rwfile: every 128 KiB chunk must match its owner's pattern exactly
CS=131072
NCH=64
ci=0
while [ $ci -lt $NCH ]; do
    T=$([ $((ci % 2)) -eq 0 ] && echo A || echo B)
    yes "$T:$ci" | head -c $CS > /tmp/efs/mc-expect.$ci
    dd if="$M/rwfile" bs=$CS count=1 skip=$ci status=none 2>/dev/null | \
        cmp -s - /tmp/efs/mc-expect.$ci || bad "rwfile chunk $ci != $T pattern"
    rm -f /tmp/efs/mc-expect.$ci
    ci=$((ci + 1))
done

# 4. rchurn: no leftover .a/.b, remaining files readable
LEFT=$(ls "$M/rchurn" | grep -c '\.[ab]$' || true)
[ "$LEFT" -eq 0 ] || bad "rchurn has $LEFT leftover .a/.b files"

# 5. growfiles: both present and >= 640k
for T in A B; do
    S=$(stat -c %s "$M/growfile.$T" 2>/dev/null || echo 0)
    [ "$S" -ge 655360 ] || bad "growfile.$T size $S < 640k"
done

timeout 10 ls "$M" > /dev/null || bad "mount wedged: ls timed out"

if [ "$FAIL" -eq 0 ]; then
    echo "VERIFY-OK"
else
    echo "VERIFY-FAILED ($FAIL)"
    exit 1
fi
