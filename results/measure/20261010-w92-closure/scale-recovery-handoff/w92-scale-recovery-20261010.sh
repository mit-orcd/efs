#!/usr/bin/env bash
set -euo pipefail
base=/mnt/w92-scale-big-20261010
stem=scale-4294967296-bounded-24g-v4
marker=$base/exit-4294967296-bounded-24g-v4
out=$base/$stem-kernel-recovery
# This private run owns its source until the terminal marker is written.
while [[ ! -f "$marker" ]]; do
  if ! ps -p 6246 -o args= | grep -Fq '/home/efs/w92-query-scale-sequential'; then
    printf '%s\n' 'original process missing without terminal marker; recovery not started' > "$out.skipped"
    exit 1
  fi
  sleep 60
done
if [[ $(cat "$marker") != 0 ]]; then
  printf '%s\n' 'original acceptance run failed; recovery not started' > "$out.skipped"
  exit 1
fi
python3 - "$base/$stem.log" <<'PY'
import json,sys
rows=[]
for line in open(sys.argv[1]):
    if not line.startswith('{'): continue
    rows.append(json.loads(line))
for phase,number in [('populated',1),('build',2),('cold',1),('mutation',1)]:
    found=[r for r in rows if r.get('phase')==phase]
    assert len(found)==number and all(r['rc']==0 and r['files']==4294967296 for r in found), (phase,found)
PY
{
  date -u
  grep -E '^(MemAvailable|Cached|Buffers):' /proc/meminfo
  sync
  echo 3 > /proc/sys/vm/drop_caches
  date -u
  grep -E '^(MemAvailable|Cached|Buffers):' /proc/meminfo
} > "$out.cache-state"
ulimit -n 524288
ulimit -c 0
set +e
runuser -u efs -- env EFS_QUERY_DISK_RESERVE_MIB=102400 \
  EFS_QUERY_DISK_TOTAL_MIB=16777216 EFS_QUERY_DISK_MAX_MIB=8388608 \
  bash -c 'ulimit -n 524288; ulimit -c 0; exec nice -n 19 ionice -c 3 /usr/bin/time -v -o "$1" /home/efs/w92-query-scale-full-oracle "$2" 4294967296 --recover' \
  w92-recovery "$out.time" "$base/$stem" \
  > "$out.log" 2>&1
result=$?
set -e
printf '%s\n' "$result" > "$out.exit"
date -u > "$out.finished"
chown efs:efs "$out.cache-state" "$out.log" "$out.time" "$out.exit" "$out.finished"
exit "$result"
