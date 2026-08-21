#!/bin/bash
# mc_stress_worker.sh <mount> <tag(A|B)> <iters>
# One client's half of the multi-client concurrent stress. Run with tag A on
# one client and tag B on another, concurrently; then mc_stress_verify.sh.
#
# Phases:
#  1. same-dir concurrent creates (cdir/)
#  2. same-file O_APPEND from both clients (appfile)
#  3. same-file disjoint 128 KiB chunks, interleaved (rwfile) — chunk-aligned
#     so no cross-client sub-chunk RMW (a known follow-on) is exercised
#  4. rename churn in a shared dir (rchurn/)
#  5. cross-client read-during-write (growfile.A/B)
set -u
M="$1"
T="$2"
N="${3:-100}"
FAIL=0
note() { echo "[$T] $*"; }
bad() { echo "[$T] FAIL: $*"; FAIL=$((FAIL+1)); }

mkdir -p "$M/cdir" "$M/rchurn"

# 1. same-dir creates
for i in $(seq 1 "$N"); do
    echo "$T-$i" > "$M/cdir/$T-$i" 2>/dev/null || bad "create cdir/$T-$i rc=$?"
done
note "creates done"

# 2. same-file O_APPEND (line atomicity)
for i in $(seq 1 "$N"); do
    echo "$T-$i" >> "$M/appfile" 2>/dev/null || { bad "append $i"; break; }
done
note "appends done"

# 3. disjoint 128 KiB chunks of a shared 8 MiB file (A=even chunks, B=odd)
CS=131072
NCH=64
TMP="/tmp/efs/mc-chunk.$T"
for round in 1 2 3; do
    ci=0
    while [ $ci -lt $NCH ]; do
        if [ $(( (ci + $( [ "$T" = A ] && echo 0 || echo 1 )) % 2 )) -eq 0 ]; then
            yes "$T:$ci" | head -c $CS > "$TMP"
            dd if="$TMP" of="$M/rwfile" bs=$CS seek=$ci conv=notrunc \
                status=none 2>/dev/null || { bad "chunk write ci=$ci"; break; }
        fi
        ci=$((ci + 1))
    done
done
rm -f "$TMP"
note "chunk writes done"

# 4. rename churn
for i in $(seq 1 "$N"); do
    f="$M/rchurn/$T-f$((i % 16))"
    echo "$T$i" > "$f.a" 2>/dev/null
    mv "$f.a" "$f" 2>/dev/null
    mv "$f" "$f.b" 2>/dev/null
    mv "$f.b" "$f" 2>/dev/null
done
note "rename churn done"

# 5. watch the other client's growing file; size must never go backwards
OTHER=$([ "$T" = "A" ] && echo B || echo A)
prev=0
for i in $(seq 1 20); do
    if [ -e "$M/growfile.$OTHER" ]; then
        S=$(stat -c %s "$M/growfile.$OTHER" 2>/dev/null || echo -1)
        [ "$S" -ge "$prev" ] || bad "growfile.$OTHER shrank $prev -> $S"
        [ "$S" -ge 0 ] && prev=$S
    fi
    sleep 0.2
done
for i in $(seq 1 10); do
    dd if=/dev/zero bs=64k count=1 status=none 2>/dev/null >> "$M/growfile.$T"
done
note "cross-read done"

: > "$M/done.$T"
if [ "$FAIL" -eq 0 ]; then
    note "WORKER-OK"
else
    note "WORKER-FAILED ($FAIL)"
    exit 1
fi
