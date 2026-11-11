#!/bin/bash
# Task V3: instruction count per function of one source file, inlining and
# inter-procedural optimisation off (the flags of scripts/evidence/dl/
# instruction_stream.sh), from the Makefile's own compile command.
# usage: instruction_counts.sh TREE SOURCE.c OUTFILE func...
# Counts the instruction lines of `objdump -d --no-show-raw-insn`, alignment
# nop lines excluded (they are padding the linker places). Prints "func N".
set -eu
T=$(cd "$1" && pwd); SRC=$2; OUT=$3; shift 3
X="-fno-inline -fno-inline-small-functions -fno-inline-functions-called-once -fno-partial-inlining
   -fno-ipa-cp -fno-ipa-sra -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-reference -fno-ipa-modref
   -fno-ipa-vrp -fno-ipa-bit-cp -fno-ipa-ra -fno-ipa-profile -fno-ipa-stack-alignment
   -fno-ipa-cp-clone -fno-optimize-sibling-calls"
X=$(echo $X)
obj=${SRC%.c}.o
cmd=$(cd "$T" && make -n -B "$obj" | grep -- "-c -o $obj")
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
(cd "$T" && eval "${cmd/-c -o $obj/$X -c -o $D/x.o}")
objdump -d --no-show-raw-insn "$D/x.o" > "$D/dis.txt"
for f in "$@"; do
    n=$(awk -v f="$f" '
        $0 ~ "^[0-9a-f]+ <" f ">:$" {on=1; next}
        /^[0-9a-f]+ <.*>:$/ {on=0}
        on && /^ +[0-9a-f]+:/ { if ($0 !~ /\tnop|\txchg +%ax,%ax|\tcs nopw|\tdata16/) c++ }
        END {print c+0}' "$D/dis.txt")
    echo "$f $n"
done | tee "$OUT"
