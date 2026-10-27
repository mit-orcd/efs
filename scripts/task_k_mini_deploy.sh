#!/bin/bash
set -euo pipefail
a=/data1/efs/evidence/taskK-orcd-mini-20261027
exec > "$a/deployment.log" 2>&1
trap 'echo $? > "$a/deployment-exit"' EXIT
cd /data1/efs/devops
/data1/efs/src/efsd --version > "$a/previous-identity.txt"
cp clusters/orcd-mini.env "$a/previous-orcd-mini.env"
/data1/efs/src/efs-mgmt status 10.1.99.1:17432 > "$a/previous-status.txt"
/data1/efs/src/efs-mgmt raft-status 10.1.99.1:17432 > "$a/previous-raft-status.txt"
# Clean the configured local mount before invoking lifecycle (which otherwise
# contains a forced-discard fallback). The remote client was stopped first.
/tmp/efs-taskK-mini-20261027/source/scripts/client.sh stop /data1/efs/mnt
! mountpoint -q /data1/efs/mnt
EFS_CLUSTER=orcd-mini ./stop.sh
for i in 1 2 3; do
 test -f /data1/efs/n$i/log/efsd.exit
 cp /data1/efs/n$i/log/efsd.exit "$a/previous-n$i-exit.txt"
done
# These archives close before formatting, retaining original daemon logs,
# PID/ownership/start records and historical test summaries. No store payloads.
tar -czf "$a/previous-logs.tar.gz" -C /data1/efs logs
cp /data1/efs/efs-fuse-mnt.log "$a/previous-local-client.log" 2>/dev/null || true
sha256sum "$a/previous-logs.tar.gz" > "$a/previous-logs.sha256"
tar -tzf "$a/previous-logs.tar.gz" > "$a/previous-logs.contents"
# Only the three user-authorized product stores; private Task H is unrelated.
python3 - <<'PY'
from pathlib import Path
for proc in Path('/proc').glob('[0-9]*'):
 try: args=(proc/'cmdline').read_bytes().split(b'\0')
 except OSError: continue
 assert not any(x in args for x in (b'/data1/efs/n1',b'/data1/efs/n2',b'/data1/efs/n3',b'/data1/efs/n4')),str(proc)
assert not Path('/data1/efs/n4').exists(),'unexpected pre-existing n4; retain for inspection'
PY
for i in 1 2 3; do "$HOME/bin/edelete" --force --delete /data1/efs/n$i; done
rm -f /data1/efs/.mkfs-done
test ! -e /data1/efs/releases/00e85a299531
mv /tmp/efs-taskK-mini-20261027/source /data1/efs/releases/00e85a299531
ln -sfn /data1/efs/releases/00e85a299531 /data1/efs/src
EFS_CLUSTER=orcd-mini EFS_START_TIMEOUT=900 ./start.sh
/data1/efs/src/efs-mgmt status 10.1.99.1:17432 > "$a/formed-three-status.txt"
/data1/efs/src/efs-mgmt raft-status 10.1.99.1:17432 > "$a/formed-three-raft.txt"
echo 'THREE MEMBERS RUNNING; fourth not added yet'
