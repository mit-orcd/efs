#!/bin/bash
set -euo pipefail
exec 9>/data1/efs/amd-live-reservation/lock
flock -n 9 || exit 2
printf '%s\n' 'devel-w66 bounded reap private direct/TCP GC and POSIX active' > /data1/efs/amd-live-reservation/owner
export EFS_CLUSTER=amd EFS_TRANSPORT=tcp EFS_DIRECT_IO=1 EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
restore() {
 /data1/efs/devops/start.sh 9>&- >> /tmp/efs-w66-reap-restore.log 2>&1 || return 1
 printf '%s\n' 'released: devel-w66 ab050595 installed direct/TCP; private bounded reap fixtures stopped, both mounts restored' > /data1/efs/amd-live-reservation/owner
}
trap restore EXIT
/data1/efs/src/scripts/client.sh stop /data1/efs/mnt > /tmp/efs-w66-reap-borrow.log 2>&1
/data1/efs/src/scripts/client.sh stop /data1/efs/mnt2 >> /tmp/efs-w66-reap-borrow.log 2>&1
mkdir -p /data1/efs/w66-gc-fixtures
cd /data1/efs/w66-active-src-20261009
python3 tests/live/gc_reclamation.py --mode direct --transport tcp --root /data1/efs/w66-gc-fixtures --root /data1/efs/w66-gc-fixtures --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --port 27640 --posix --append-audit 9>&- > /tmp/efs-w66-reap-live.log 2>&1
