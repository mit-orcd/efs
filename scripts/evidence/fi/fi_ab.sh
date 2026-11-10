#!/bin/bash
# fi_ab.sh OUTROOT LABEL=TREE[=G0NODE:G2NODE] ... — Task FI interleaved A/B: one fresh-cluster
# fi_run.sh per argument, in order (base, devel, base, devel, ...). The label
# names OUTROOT/<n>-<label>; ports rotate in 18400-18490. Summary lines go to
# OUTROOT/summary.txt as they finish. Run it detached in screen.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); O=$1; shift; n=0
mkdir -p "$O"
for a in "$@"; do
  n=$((n+1)); l=${a%%=*}; t=${a#*=}; pl=""; case $t in *=*) pl=${t#*=}; t=${t%%=*}; l=$l-p${pl/:/_};; esac
  FI_PLACE=$pl bash "$HERE/fi_run.sh" "$(printf '%02d' $n)-$l" "$t" "$O" $((18400 + (n % 9) * 10)) "${FI_EASY_MB:-2048}"
  echo "$(printf '%02d' $n)-$l rc=$? $(grep -ahE '^write ' "$O/$(printf '%02d' $n)-$l/ior.txt" | awk '{print "write_MiBs=" $2}') $(grep -ahE '^read ' "$O/$(printf '%02d' $n)-$l/ior.txt" | awk '{print "read_MiBs=" $2}') $(grep -h load_before "$O/$(printf '%02d' $n)-$l/meta.txt" | cut -d' ' -f2-)" >> "$O/summary.txt"
done
echo "done $(date -u +%FT%TZ)" >> "$O/summary.txt"
