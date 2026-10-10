#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-repair
for node in "${NODES[0]}" "$GW"; do
 if stop_client "$node" /mnt/efs-i > "$out/final-batch-drain-$node-i.log" 2>&1; then
   printf '%s:/mnt/efs-i\t0\n' "$node" >> "$out/final-batch-drains.tsv"
 else
   printf '%s:/mnt/efs-i\t1\n' "$node" >> "$out/final-batch-drains.tsv"
   exit 1
 fi
done
ssh_node_once "$GW" 'cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-i -o allow_other' > "$out/final-batch-remount-i.log" 2>&1
ssh_node_once "$GW" 'sudo -n -H -u efs python3 -u /var/tmp/efs-cold-large-verify.py /mnt/efs-i' > "$out/final-batch-cold-large.log" 2>&1
if stop_client "$GW" /mnt/efs-h > "$out/drain-superseded-h.log" 2>&1; then
 printf '/mnt/efs-h\tretired\n' >> "$out/final-batch-legacy-drains.tsv"
else
 printf '/mnt/efs-h\tpreserved-refused\n' >> "$out/final-batch-legacy-drains.tsv"
fi
if stop_client "$GW" /mnt/efs-b > "$out/drain-old-b-last-retry.log" 2>&1; then
 printf '/mnt/efs-b\tretired\n' >> "$out/final-batch-legacy-drains.tsv"
else
 printf '/mnt/efs-b\tpreserved-refused\n' >> "$out/final-batch-legacy-drains.tsv"
fi
if ssh_node_once "$GW" 'set -eo pipefail
trap "sudo -n exportfs -r" EXIT
sudo -n exportfs -u "*:/mnt/efs"
sudo -n exportfs -f
cd /tmp/efs
sudo -n -H -u efs ./scripts/client.sh stop /mnt/efs
sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other' > "$out/primary-last-retry.log" 2>&1; then
 printf '/mnt/efs\trefreshed\n' >> "$out/final-batch-legacy-drains.tsv"
else
 printf '/mnt/efs\tpreserved-refused-export-restored\n' >> "$out/final-batch-legacy-drains.tsv"
fi
for node in "${NODES[@]}" "$GW"; do
 echo "$node"
 ssh_node_once "$node" 'ps -C efsd,efs-fuse -o user,pid,args; sudo -n -H -u efs /tmp/efs/efsd --version; sudo -n -H -u efs sha256sum /tmp/efs/efsd /tmp/efs/efs-fuse'
done > "$out/final-batch-runtime.log"
ssh_node_once "${NODES[0]}" 'sudo -n -H -u efs /tmp/efs/efs-mgmt status 192.168.2.11:17432' > "$out/final-batch-health.log"
