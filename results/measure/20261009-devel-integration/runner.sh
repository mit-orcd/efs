#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/amd-live-reservation/lock
flock -n 9 || { echo 'AMD reservation busy'; exit 2; }
printf '%s\n' 'integration-final-20261009 devel 7b4f6085 combined gates active' > /data1/efs/amd-live-reservation/owner
out=/data1/efs/logs/integration-final-20261009
mkdir -p "$out"
export EFS_CLUSTER=amd EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_DIRECT_IO=1
cd /data1/efs/devops
cp -a --reflink=auto /data1/efs/src /data1/efs/integration-final-backup-20261009
restore() {
 ./stop.sh >> "$out/restore.log" 2>&1 || { echo 'Clean rollback stop refused; retained stores and binaries'; return 1; }
 rsync -a --delete /data1/efs/integration-final-backup-20261009/ /data1/efs/src/ >> "$out/restore.log" 2>&1 || return 1
 EFS_DIRECT_IO=1 ./start.sh 9>&- >> "$out/restore.log" 2>&1 || return 1
 printf '%s\n' 'released: combined devel 7b4f6085 gates complete; original installed source/binaries restored direct/TCP' > /data1/efs/amd-live-reservation/owner
}
trap restore EXIT
./stop.sh > "$out/stop.log" 2>&1
rsync -a --delete /data1/efs/integration-final-src-20261009/ /data1/efs/src/
failed=0
for mode in direct buffered; do
 if [ "$mode" = direct ]; then export EFS_DIRECT_IO=1; else export EFS_DIRECT_IO=0; fi
 ./start.sh 9>&- > "$out/$mode-start.log" 2>&1
 for suite in smoke smoke2 smoke-persist; do
  rc=0
  ./$suite.sh 9>&- > "$out/$mode-$suite.log" 2>&1 || rc=$?
  printf '%s\t%s\t%s\n' "$mode" "$suite" "$rc" | tee -a "$out/verdicts.tsv"
  [ "$rc" = 0 ] || failed=1
 done
 ./stop.sh > "$out/$mode-stop.log" 2>&1
 done
exit "$failed"
