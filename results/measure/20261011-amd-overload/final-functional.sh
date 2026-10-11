#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-overload-20261011
src=/data1/efs/devel-amd-overload-20261011
while [ ! -f "$out/long-queue.exit" ]; do sleep 15; done
trap 'echo $? > "$out/final-functional.exit"' EXIT
[ "$(cat "$out/long/restore.exit")" = 0 ]
python3 - <<'PY'
import json,re
from pathlib import Path
b=Path('/data1/efs/amd-overload-20261011/long')
for mode in ['direct','buffered']:
 p=b/f'admission-{mode}'
 assert (p/'run.exit').read_text().strip()=='0'
 files=list(p.glob('gc-*/tiny-sustained.json'));assert len(files)==1
 r=json.loads(files[0].read_text())
 assert not r['overload_errors'] and r['wall_s']>=600
 assert r['cold_verified'] and r['physical_inventory_empty'] and r['post_pressure_cold_write_verified']
 ss=r['process_samples'];last=ss[-1];earlier=min(ss,key=lambda s:abs(s['seconds']-450))
 for key in ['tiny_closed_by_worker','large_synced_writes_by_worker']:
  assert all(a>z for a,z in zip(last[key],earlier[key])),(mode,key)
 assert any('data-gc-admission busy=' in log.read_text() for log in files[0].parent.glob('n*.log'))
for line in Path('/data1/efs/amd-overload-20261011/long-status-samples.jsonl').read_text().splitlines():
 for node in json.loads(line)['nodes']:
  for value in re.findall(r'data_usage_bytes=(\d+)',node.get('status','')):assert int(value)<=4<<30
PY
mkdir -p "$out/final-functional"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/final-functional/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 10 stat "$m" >> "$out/final-functional/restore.log" 2>&1 || restored=1
 done
 echo "$restored" > "$out/final-functional/restore.exit"
 echo "$rc" > "$out/final-functional.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/final-functional/borrow.log" 2>&1
done
cd "$src"
python3 -u tests/live/gc_reclamation.py --mode direct --transport tcp \
 --root "$out/final-functional" --root "$out/final-functional" --port 24210 --fixture-quota 4G \
 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
 --guarded-leases --posix > "$out/final-functional/run.log" 2>&1
