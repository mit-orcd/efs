#!/bin/bash
# Task V4 stage B: instruction counts (inlining off, scripts/evidence/v3/
# instruction_counts.sh) and the address-free disassembly of raft_host.o,
# group_view.o and (when it exists) read_proof.o, for one tree.
# usage: instruction_run.sh TREE OUTDIR LABEL FUNCTIONS.txt [READ_PROOF_FUNCTIONS.txt]
#   FUNCTIONS.txt        functions counted in src/server/raft_host.c
#   READ_PROOF_FUNCTIONS.txt  functions counted in src/server/read_proof.c (after only)
# Writes OUTDIR/counts_LABEL.txt, OUTDIR/disasm_LABEL_<object>.txt and
# OUTDIR/disasm_LABEL.sha256. The tree must already be built with `make`.
set -eu
T=$(cd "$1" && pwd); O=$2; L=$3; F=$4; R=${5:-}
S=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O"
: > "$O/counts_$L.txt"
"$S/../v3/instruction_counts.sh" "$T" src/server/raft_host.c "$O/c1.tmp" $(grep -v '^#' "$F") >/dev/null
sed 's/^/raft_host.c /' "$O/c1.tmp" >> "$O/counts_$L.txt"; rm -f "$O/c1.tmp"
if [ -n "$R" ]; then
    "$S/../v3/instruction_counts.sh" "$T" src/server/read_proof.c "$O/c2.tmp" $(grep -v '^#' "$R") >/dev/null
    sed 's/^/read_proof.c /' "$O/c2.tmp" >> "$O/counts_$L.txt"; rm -f "$O/c2.tmp"
fi
: > "$O/disasm_$L.sha256"
for ob in src/server/raft_host.o src/server/group_view.o src/server/read_proof.o; do
    [ -f "$T/$ob" ] || continue
    n=$(basename "$ob" .o)
    objdump -d --no-show-raw-insn "$T/$ob" |
        sed -E 's/^[[:space:]]*[0-9a-f]+:[[:space:]]*//; s/^[0-9a-f]+ </</; s/ [0-9a-f]+ <([^>]*)>/ <\1>/' > "$O/disasm_${L}_$n.txt"
    (cd "$O" && sha256sum "disasm_${L}_$n.txt" >> "disasm_$L.sha256"; wc -l < "disasm_${L}_$n.txt" | sed "s/^/lines $n: /" >> "disasm_$L.sha256")
done
