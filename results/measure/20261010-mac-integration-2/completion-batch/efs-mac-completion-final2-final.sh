#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261010-mac-integration-2/completion-batch
stop_client "${NODES[0]}" /mnt/efs > "$out/peer-seed-stop.log" 2>&1
ssh_node_once "$GW" 'set -eo pipefail
trap "sudo -n exportfs -r" EXIT
sudo -n exportfs -u "*:/mnt/efs"
sudo -n exportfs -f
cd /tmp/efs
sudo -n -H -u efs ./scripts/client.sh stop /mnt/efs
sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other
sudo -n -H -u efs python3 -u /var/tmp/efs-cold-large-verify.py /mnt/efs' > "$out/primary-final-remount.log" 2>&1
for node in "${NODES[@]}" "$GW"; do
 echo "$node"
 ssh_node_once "$node" 'ps -C efsd,efs-fuse -o user,pid,args; sudo -n -H -u efs /tmp/efs/efsd --version; for pid in $(pgrep -x efs-fuse); do sudo -n -H -u efs /proc/$pid/exe --version; done; sudo -n -H -u efs sha256sum /tmp/efs/efsd /tmp/efs/efs-fuse'
done > "$out/runtime-final.log"
ssh_node_once "${NODES[0]}" 'sudo -n -H -u efs /tmp/efs/efs-mgmt status 192.168.2.11:17432' > "$out/health-final.log"
