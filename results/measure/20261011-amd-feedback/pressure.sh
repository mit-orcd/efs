#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-feedback-20261011
src=/data1/efs/devel-amd-feedback-20261011
while [ ! -f "$out/functional/round.exit" ]; do sleep 15; done
[ "$(cat "$out/functional/round.exit")" = 0 ]
[ "$(cat "$out/functional/restore.exit")" = 0 ]
mkdir -p "$out/pressure"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_DATA_GC_ADMISSION=1 EFS_GC_BACKLOG_IDLE_MS=20
export EFS_RAFT_FLOW_EARLY_RETIRE=0 EFS_RAFT_GROUP_DRAIN=0 EFS_BATCH_OBS=1 EFS_RAFT_OBS=1
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/pressure/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 15 stat "$m" >> "$out/pressure/restore.log" 2>&1 || restored=1
 done
 echo "$restored" > "$out/pressure/restore.exit"
 echo "$rc" > "$out/pressure/round.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
cd "$src"
python3 tests/test_raft_slow_sync.py > "$out/pressure/slow-sync-unit.log" 2>&1
python3 tests/test_fuse_lease_retention.py > "$out/pressure/create-alloc-unit.log" 2>&1
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/pressure/borrow.log" 2>&1
done
for cell in direct-off-1 direct-on-1 direct-on-2 direct-off-2 direct-off-3 direct-on-3 buffered-on-1 direct-on-unobserved; do
 mode=${cell%%-*};export EFS_RAFT_FLOW=0 EFS_BATCH_OBS=1 EFS_RAFT_OBS=1
 [[ $cell != *-on-* ]] || export EFS_RAFT_FLOW=1
 [[ $cell != *-unobserved ]] || export EFS_BATCH_OBS=0 EFS_RAFT_OBS=0
 dir=$out/pressure/$cell;mkdir -p "$dir"
 printf '%s %s START\n' "$(date -u +%FT%TZ)" "$cell" >> "$out/pressure/progress.log"
 python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp \
  --root "$dir" --root "$dir" --port 24210 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
  --replication-pressure --tiny-sustained-seconds 180 --tiny-sustained-large-mib-s 0 --tiny-overload \
  > "$dir/run.log" 2>&1
 echo 0 > "$dir/run.exit"
 python3 - "$dir" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]);files=list(p.glob('gc-*/tiny-sustained.json'));assert len(files)==1
r=json.loads(files[0].read_text());assert not r['overload_errors'],r['overload_errors']
assert r['cold_verified'] and r['physical_inventory_empty'] and r['post_pressure_cold_write_verified'] and r['replication_catchup_verified']
last=r['process_samples'][-1];first=min(r['process_samples'],key=lambda x:abs(x['seconds']-60))
for key in ['tiny_closed_by_worker','large_synced_writes_by_worker']:
 assert all(a>b for a,b in zip(last[key],first[key])),(key,last[key],first[key])
PY
 printf '%s %s PASS\n' "$(date -u +%FT%TZ)" "$cell" >> "$out/pressure/progress.log"
done
