#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-overload-20261011
while [ ! -f "$out/queue.exit" ]; do sleep 15; done
trap 'echo $? > "$out/long-queue.exit"' EXIT
[ "$(cat "$out/live/restore.exit")" = 0 ]
python3 - <<'CHECK'
import json
from pathlib import Path
b=Path('/data1/efs/amd-overload-20261011/live')
for mode in ['direct','buffered']:
 p=b/f'admission-{mode}'
 assert (p/'run.exit').read_text().strip()=='0'
 files=list(p.glob('gc-*/tiny-sustained.json'));assert len(files)==1
 r=json.loads(files[0].read_text())
 assert not r['overload_errors'] and r['wall_s']>=r['requested_seconds']
 assert r['cold_verified'] and r['physical_inventory_empty']
CHECK
cp "$out/new-fixtures/gc_reclamation.py" "$out/new-fixtures/tiny_sustained.py" /data1/efs/devel-amd-overload-20261011/tests/live/
sha256sum /data1/efs/devel-amd-overload-20261011/tests/live/{gc_reclamation,tiny_sustained}.py > "$out/long-fixture-sha256.txt"
bash "$out/long.sh"
