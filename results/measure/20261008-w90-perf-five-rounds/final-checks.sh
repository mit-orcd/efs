#!/bin/bash
set -euo pipefail
root=/data1/efs/bench/w90-perf5-20261008
while ! grep -q 'CONFIRM_PASS round5-final' "$root/confirm.log" 2>/dev/null; do sleep 5; done
cd "$root/src"
make -j4 efsd > "$root/final-build-efsd.log" 2>&1
for t in test_storage_churn test_bench_churn test_storage_ranges test_bench_ranges test_bench_cli test_storage_principles test_bench_profile; do
 TMPDIR="$root/scratch" python3 "tests/$t.py" > "$root/final-$t.log" 2>&1
done
python3 - <<'PY'
from pathlib import Path
import zipfile,hashlib,json
root=Path('/data1/efs/bench/w90-perf5-20261008')
assert not list((root/'scratch').iterdir()),list((root/'scratch').iterdir())
with zipfile.ZipFile(root/'final-sources.zip','w',zipfile.ZIP_DEFLATED) as z:
 for f in ['src/bench/storage_churn.c','src/bench/storage_churn.h','src/bench/perf_control.h','src/bench/latency.h','src/client/efs_bench.c','Makefile','tests/test_storage_churn.py']:
  z.write(root/'src'/f,f)
(root/'acceptance.txt').write_text('Final benchmark/daemon builds and targeted benchmark tests passed. Scratch empty.\n')
PY
printf 'FINAL_CHECKS_PASS\n'
