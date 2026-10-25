#!/bin/bash
# flake_loop.sh TREE OUT LABEL N CMD...: run CMD N times from TREE with a
# private TMPDIR per iteration; keep stdout/stderr and, on failure, the
# iteration's logs (the tests' *_KEEP variables keep their work trees; only
# *.log, *.json and *.txt under 50 MB are copied) under OUT/LABEL/fail-NN/.
# One line per iteration in OUT/LABEL/loop.log; the last line is the tally.
TREE=$1; OUT=$2; LABEL=$3; N=$4; shift 4
D="$OUT/$LABEL"; mkdir -p "$D"; cd "$TREE" || exit 2
export EFS_K33_KEEP=1 EFS_B6_KEEP=1 EFS_K35_KEEP=1 EFS_K36_KEEP=1 EFS_K37_KEEP=1 EFS_RSD_KEEP=1 EFS_D4_KEEP=1 EFS_QFIRST_KEEP=1
fails=0
for i in $(seq 1 $N); do
    t="$TREE/tmp/loop-$LABEL-$i"; [ -d "$t" ] && ~/bin/edelete --force --delete "$t"; mkdir -p "$t"
    t0=$(date +%s)
    TMPDIR="$t" "$@" > "$D/run-$i.out" 2>&1; rc=$?
    echo "$LABEL $i rc=$rc $(( $(date +%s) - t0 ))s $(tail -1 "$D/run-$i.out" | cut -c1-160)" >> "$D/loop.log"
    if [ $rc -ne 0 ]; then fails=$((fails+1)); mkdir -p "$D/fail-$i"; mv "$D/run-$i.out" "$D/fail-$i/out.txt"
        # keep logs only (no fragment stores)
        (cd "$t" && find . -type f \( -name '*.log' -o -name '*.json' -o -name '*.txt' -o -name 'efsd.*' \) -size -50M | cpio -pdm "$D/fail-$i" 2>/dev/null)
    fi
    ~/bin/edelete --force --delete "$t"
done
echo "$LABEL done fails=$fails/$N" >> "$D/loop.log"
