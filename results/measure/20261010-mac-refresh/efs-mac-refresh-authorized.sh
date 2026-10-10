#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261010-mac-refresh
ssh_node_once "$GW" 'set -eo pipefail
trap "sudo -n exportfs -r" EXIT
sudo -n exportfs -u "*:/mnt/efs"
sudo -n exportfs -f
cd /tmp/efs
sudo -n -H -u efs ./scripts/client.sh stop --force-discard /mnt/efs-b
sudo -n -H -u efs ./scripts/client.sh stop --force-discard /mnt/efs
sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other
sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-b -o allow_other
ps -C efs-fuse -o user,pid,args' > "$out/authorized-refresh.log" 2>&1
stop_client "$GW" /mnt/efs-i > "$out/retire-validation-i.log" 2>&1
ssh_node_once "${NODES[0]}" 'set -e; sudo -n install -d -m 0755 -o efs -g efs /mnt/efs; sudo -n touch /mnt/.efs-efs.lock /mnt/efs-fuse-efs.log; sudo -n chown efs:efs /mnt/.efs-efs.lock /mnt/efs-fuse-efs.log; cd /tmp/efs; sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other' > "$out/seed-peer-mount.log" 2>&1
for node in "${NODES[@]}" "$GW"; do
 echo "$node"
 ssh_node_once "$node" 'ps -C efsd,efs-fuse -o user,pid,args; sudo -n -H -u efs /tmp/efs/efsd --version'
done > "$out/runtime-start.log"
