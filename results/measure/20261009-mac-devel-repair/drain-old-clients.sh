#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-repair
for mount in /mnt/efs-g /mnt/efs-b /mnt/efs-d /mnt/efs-c /mnt/efs-e; do
 name=${mount##*/}
 if stop_client "$GW" "$mount" > "$out/drain-old-$name.log" 2>&1; then
   printf '%s\tretired\n' "$mount" >> "$out/old-client-drains.tsv"
 else
   printf '%s\tpreserved-refused\n' "$mount" >> "$out/old-client-drains.tsv"
 fi
done
if stop_client "${NODES[0]}" /mnt/efs-g > "$out/drain-old-seed-g.log" 2>&1; then
 printf 'efs1:/mnt/efs-g\tretired\n' >> "$out/old-client-drains.tsv"
else
 printf 'efs1:/mnt/efs-g\tpreserved-refused\n' >> "$out/old-client-drains.tsv"
fi
if ssh_node_once "$GW" 'set -eo pipefail
sudo -n exportfs -u "*:/mnt/efs"
sudo -n exportfs -f
cd /tmp/efs
if sudo -n -H -u efs ./scripts/client.sh stop /mnt/efs; then
  sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other
  sudo -n exportfs -r
else
  result=$?
  sudo -n exportfs -r
  exit "$result"
fi' > "$out/primary-refresh-latest.log" 2>&1; then
 printf '/mnt/efs\trefreshed\n' >> "$out/old-client-drains.tsv"
else
 printf '/mnt/efs\tpreserved-refused-export-restored\n' >> "$out/old-client-drains.tsv"
fi
