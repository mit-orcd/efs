#!/bin/bash
# posix9_none_count.sh — W8: a 9-host parallel posix run hits the 385 s
# python cap and reports "[None]" for tests that never ran. This counts,
# per host TSV in a results/posix/<run> directory, how many tests ran vs
# never ran vs failed, so the run is read as what it measured and not as
# "60 bugs". Read-only; no cluster access.
#
#   bash tests/measure/posix9_none_count.sh results/posix/<run-id>
#
# To produce such a run (from node9901 via efs-bg.sh, ~7 min):
#   EFS_TRANSPORT=tcp bash tests/run_tests.sh posix --parallel fcstor007.ib fcstor008.ib ... fcstor015.ib
# W8's fix is in the HARNESS (tests/posix/posix_suite.py: write the TSV
# incrementally / per test, so a cut run still has a row per finished test
# and an explicit NOTRUN for the rest) — not in efsd or efs-fuse, so it does
# not change the cluster version. Gate: every host's TSV has 201 rows with
# no [None].
set -u
d=${1:?results/posix/<run-id>}
[ -d "$d" ] || { echo "no such dir $d"; exit 2; }
printf '%-12s %5s %5s %5s %5s %6s\n' host rows pass fail skip none
for f in "$d"/efs-*.tsv "$d"/*-fcstor0*.tsv; do
    [ -f "$f" ] || continue
    h=$(basename "$f" .tsv | grep -o 'fcstor0[0-9]*' | head -1)
    rows=$(grep -vc '^#\|^test' "$f")
    pass=$(awk -F'\t' '$2=="PASS"' "$f" | wc -l)
    fail=$(awk -F'\t' '$2=="FAIL"' "$f" | wc -l)
    skip=$(awk -F'\t' '$2=="SKIP"' "$f" | wc -l)
    none=$(grep -c 'None' "$f")
    printf '%-12s %5d %5d %5d %5d %6d\n' "${h:-?}" "$rows" "$pass" "$fail" "$skip" "$none"
done
echo "compare-*.txt summaries:"; grep -H 'both pass\|EFS BUGS' "$d"/compare-*.txt 2>/dev/null
