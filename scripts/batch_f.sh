#!/bin/bash
# Stage 1 batch F in the matrix's order on one host: F1 `make test-all` summary,
# F2 TSan then ASan+UBSan+LSan sanitizer lanes, F3 the leak gate. Each lane is
# timed; raw output goes to $ART (outside Git). Run from the tree root inside
# `screen -dmS`. Usage: ART=$HOME/efs-artifacts/<run> bash scripts/batch_f.sh [lane...]
# lanes: f1 tsan asan leak (default: all, in that order)
cd "$(dirname "$0")/.." || exit 2
ART=${ART:?set ART to the raw-output directory}
mkdir -p "$ART" "${TMPDIR:?set TMPDIR under \$HOME}" || exit 2
export EFS_GIT_ID=${EFS_GIT_ID:-$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)}
lanes=${*:-f1 tsan asan leak}
for l in $lanes; do
    t0=$(date +%s)
    case $l in
    f1)   EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh >"$ART/f1.out" 2>&1; rc=$?
          cp -f summary.txt timings.tsv "$ART/" 2>/dev/null; cp -rf tmp/fail-*.log "$ART/" 2>/dev/null ;;
    tsan) EFS_SAN_LOGDIR="$ART/tsan" bash tests/sanitizer_check.sh tsan >"$ART/tsan.out" 2>&1; rc=$? ;;
    asan) EFS_SAN_LOGDIR="$ART/asan" bash tests/sanitizer_check.sh asan >"$ART/asan.out" 2>&1; rc=$? ;;
    leak) EFS_LEAK_LOGDIR="$ART/leak" bash tests/leak_check.sh >"$ART/leak.out" 2>&1; rc=$? ;;
    *) echo "unknown lane $l" >&2; exit 2 ;;
    esac
    echo "$l rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a "$ART/lanes.txt"
done
