#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
seed=${NODES[0]}
rsync_to "$seed" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/
ssh_node_once "$seed" 'set -e; ! mountpoint -q /mnt/efs-h; sudo -n install -d -m 0755 -o efs -g efs /mnt/efs-h; sudo -n touch /mnt/.efs-efs-h.lock /mnt/efs-fuse-efs-h.log; sudo -n chown efs:efs /mnt/.efs-efs-h.lock /mnt/efs-fuse-efs-h.log; if ! grep -Eq "^[[:space:]]*user_allow_other([[:space:]]*(#.*)?)?$" /etc/fuse.conf; then printf "\nuser_allow_other\n" | sudo -n tee -a /etc/fuse.conf >/dev/null; fi; cd /tmp/efs; sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-h -o allow_other'
