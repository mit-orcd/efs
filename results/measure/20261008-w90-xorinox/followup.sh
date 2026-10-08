#!/bin/bash
set -euo pipefail
root=/data1/efs/bench/w90-20261008
cd "$root/src"
for i in $(seq 1 120); do
  if python3 -c 'import json,sys; r=json.load(open(sys.argv[1]));sys.exit(0 if len(r)==18 and all(c["status"]=="PASS" for c in r) else 1)' "$root/matrix-128k/results.json"; then break; fi
  sleep 5
  if [ "$i" = 120 ]; then exit 1; fi
done
run() {
  name=$1; shift
  ./efs-bench.sh --storage-churn --storage-root "$root/scratch" --output "$root/$name" --io-modes direct --steps 24000 "$@" > "$root/$name.log" 2>&1
  tail -3 "$root/$name.log"
}
run repeat-95 --occupancies 95
run xfs-preallocate --layouts punch --occupancies 95 --preallocate
run xfs-hint-1m --layouts punch --occupancies 95 --extent-hint 1048576
run xfs-hint-8m --layouts punch --occupancies 95 --extent-hint 8388608
run xfs-filestreams --layouts punch --occupancies 95 --filestreams
run fragment-64k --occupancies 80,95 --payload-sizes 65536 --steps 48000
mkdir -p "$root/tmp"
export TMPDIR="$root/tmp"
make -j8 efsd > "$root/build-efsd.log" 2>&1
python3 tests/test_bench_cli.py > "$root/legacy-cli.log" 2>&1
python3 tests/test_storage_principles.py > "$root/legacy-principles.log" 2>&1
printf 'FOLLOWUP_PASS\n'
