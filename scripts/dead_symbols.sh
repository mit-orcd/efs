#!/bin/sh
# A12 dead-code sweep: global functions and objects defined in the product's
# objects (src/*/*.o, without src/sim and src/bench) that no product binary
# (efsd, efs-fuse, efs-mgmt, efs-bench) keeps after a --gc-sections link.
# Candidates only: a function inlined into its own file's callers also drops
# out, and a test may be its only caller. Check each with grep.
#
#   make -j8 EXTRA_DEFS="-ffunction-sections -fdata-sections" \
#        LDFLAGS="-lpthread -lm -ldl -libverbs -Wl,--gc-sections" \
#        efsd efs-fuse efs-mgmt efs-bench
#   scripts/dead_symbols.sh [TREE]     prints "SYMBOL OBJECT" per candidate
set -e
cd "${1:-.}"
tmp=$(mktemp -d)
trap 'rm -f "$tmp/defs" "$tmp/kept"; rmdir "$tmp"' EXIT
for o in $(ls src/*/*.o | grep -v -e '^src/sim/' -e '^src/bench/'); do
    nm --defined-only "$o" | awk -v o="$o" '$2 ~ /^[TDBR]$/ {print $3, o}'
done | sort -u > "$tmp/defs"
for b in efsd efs-fuse efs-mgmt efs-bench; do nm "$b" | awk '{print $NF}'; done | sort -u > "$tmp/kept"
awk 'NR==FNR {k[$1]=1; next} !($1 in k)' "$tmp/kept" "$tmp/defs" | sort -k2
