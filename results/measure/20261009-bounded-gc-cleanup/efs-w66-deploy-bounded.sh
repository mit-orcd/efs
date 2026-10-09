#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/amd-live-reservation/lock
flock -n 9 || exit 2
out=/data1/efs/logs/w66-bounded-20261009
mkdir -p "$out"
printf '%s\n' 'devel-w66 978caaf7 deployment and direct/TCP acceptance active' > /data1/efs/amd-live-reservation/owner
export EFS_CLUSTER=amd EFS_TRANSPORT=tcp EFS_DIRECT_IO=1 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
cd /data1/efs/devops
cp -a --reflink=auto /data1/efs/src /data1/efs/w66-bounded-backup-20261009
accepted=0
cleanup() {
 if [ "$accepted" = 0 ]; then
  ./stop.sh 9>&- >> "$out/rollback.log" 2>&1 || return 1
  rsync -a --delete /data1/efs/w66-bounded-backup-20261009/ /data1/efs/src/ >> "$out/rollback.log" 2>&1 || return 1
  ./start.sh 9>&- >> "$out/rollback.log" 2>&1 || return 1
 fi
 printf '%s\n' "released: devel-w66 978caaf7 deployment accepted=$accepted; direct/TCP commands disabled" > /data1/efs/amd-live-reservation/owner
}
trap cleanup EXIT
./stop.sh 9>&- > "$out/stop.log" 2>&1
rsync -a --delete /data1/efs/w66-active-src-20261009/ /data1/efs/src/
./start.sh 9>&- > "$out/start.log" 2>&1
for suite in smoke smoke2 smoke-persist; do
 ./$suite.sh 9>&- > "$out/$suite.log" 2>&1
 printf '%s\tPASS\n' "$suite" | tee -a "$out/verdicts.tsv"
done
accepted=1
