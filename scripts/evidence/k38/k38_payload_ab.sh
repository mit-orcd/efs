#!/bin/bash
# k38_payload_ab.sh FIX_TREE BASE_TREE OUTDIR PAIRS: run
# tests/test_k33_payload.py alternately from two built trees (make -j all
# tests/faults/efsd in each), PAIRS times each. Every failed run's work
# directory (daemon and client logs, stores) is kept under OUTDIR/<tree>-<n>,
# passing ones are deleted; OUTDIR/verdicts.txt gets one line per run and
# `done` at the end. Run several in parallel (one OUTDIR each) for the K38
# lanes. Raw output stays outside Git (docs/status/k38-payload-protection-
# now-20261025.md).
set -u
fix=$1 base=$2 out=$3 pairs=$4
mkdir -p "$out" "$out/tmp"
for i in $(seq 1 "$pairs"); do
  for t in fix base; do
    tree=$fix; [ "$t" = base ] && tree=$base
    log="$out/$t-$i.log"
    start=$(date -u +%FT%TZ)
    ( cd "$tree" && EFS_K33_KEEP=1 EFS_READ_FRAG_PROF=1 TMPDIR="$out/tmp" \
        timeout 900 python3 tests/test_k33_payload.py ) > "$log" 2>&1
    rc=$?
    kept=$(sed -n 's/^kept //p' "$log" | tail -1)
    if [ $rc -eq 0 ] && grep -q 'PASS' "$log"; then
      echo "$start $t $i PASS" >> "$out/verdicts.txt"
      if [ -n "$kept" ]; then
        if command -v edelete >/dev/null; then edelete --force --delete "$kept" >/dev/null
        else rm -rf "$kept"; fi
      fi
    else
      echo "$start $t $i FAIL rc=$rc" >> "$out/verdicts.txt"
      [ -n "$kept" ] && mv "$kept" "$out/$t-$i"
    fi
  done
done
echo done >> "$out/verdicts.txt"
