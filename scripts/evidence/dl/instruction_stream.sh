#!/bin/bash
# Task DL: compare the instruction stream of the unsplit meta_apply.o with the
# objects of src/meta/meta_apply/, per function (scripts/evidence/dl/objcmp.py).
#
# usage: instruction_stream.sh BEFORE_TREE AFTER_TREE OUTDIR
# Both trees built with `make all` by the same compiler (the normal objects
# are compared as built). Then each tree's module objects are rebuilt with
# inlining and inter-procedural optimisation off, from the Makefile's own
# compile command, and compared again with relocations folded into calls:
# that run shows whether any instruction changed apart from what the
# compiler chose to inline.
set -u
B=$(cd "$1" && pwd); A=$(cd "$2" && pwd); O=$3; mkdir -p "$O/noinl/before" "$O/noinl/after"
S=$(cd "$(dirname "$0")" && pwd)
python3 "$S/objcmp.py" "$B/src/meta/meta_apply.o" "$A/src/meta/meta_apply" "$O/diffs" > "$O/objcmp.txt"
X="-fno-inline -fno-inline-small-functions -fno-inline-functions-called-once -fno-partial-inlining
   -fno-ipa-cp -fno-ipa-sra -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-reference -fno-ipa-modref
   -fno-ipa-vrp -fno-ipa-bit-cp -fno-ipa-ra -fno-ipa-profile -fno-ipa-stack-alignment
   -fno-optimize-sibling-calls"
X=$(echo $X)
build() { # tree source object
    local cmd; cmd=$(cd "$1" && make -n -B "${2%.c}.o" | grep -- "-c -o ${2%.c}.o") || return 1
    (cd "$1" && eval "${cmd/-c -o ${2%.c}.o/$X -c -o $3}")
}
build "$B" src/meta/meta_apply.c "$O/noinl/before/meta_apply.o" || exit 1
for f in "$A"/src/meta/meta_apply/*.c; do
    f=${f#$A/}; build "$A" "$f" "$O/noinl/after/$(basename "$f" .c).o" || exit 1
done
OBJCMP_NORELOC=1 python3 "$S/objcmp.py" "$O/noinl/before/meta_apply.o" "$O/noinl/after" \
    "$O/noinl/diffs" > "$O/objcmp-noinline.txt"
rm -f "$O"/noinl/before/*.o "$O"/noinl/after/*.o
head -1 "$O/objcmp.txt"; head -1 "$O/objcmp-noinline.txt"
