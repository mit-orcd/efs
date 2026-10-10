#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/transfer.sh
out=$EFS_SRC/results/measure/20261010-mac-integration-2
build_id=$(cat "$out/source-build.txt" | cut -c1-12)
seed=${NODES[0]}
stage=/var/tmp/efs-devel-$build_id
ssh_node_once "$GW" 'set -eo pipefail
sudo -n exportfs -u "*:/mnt/efs"
sudo -n exportfs -f
cd /tmp/efs
for m in /mnt/efs-b /mnt/efs; do
 sudo -n -H -u efs ./scripts/client.sh stop "$m" || sudo -n -H -u efs ./scripts/client.sh stop --force-discard "$m"
done' > "$out/predeploy-client-stop.log" 2>&1
trap 'ssh_node_once "$GW" "sudo -n exportfs -r"' EXIT
ssh_node_once "$seed" 'cd /tmp/efs; sudo -n -H -u efs ./scripts/client.sh stop /mnt/efs || sudo -n -H -u efs ./scripts/client.sh stop --force-discard /mnt/efs' > "$out/predeploy-seed-stop.log" 2>&1
ssh_node_once "$seed" "set -e; next=\$(mktemp -d /tmp/efs/.next.XXXXXX); for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do cp $stage/\$b \$next/\$b; mv -f \$next/\$b /tmp/efs/\$b; done; rmdir \$next"
for node in "${NODES[@]:1}" "$GW"; do ship_binaries "$seed" "$node"; done
for node in "${NODES[@]}" "$GW"; do
 rsync_to "$node" "$EFS_SRC/scripts/" /tmp/efs/scripts/
 rsync_to "$node" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/
done
for node in "${NODES[@]}"; do
 ssh_node_once "$node" 'sudo -n systemctl restart efsd'
 for attempt in $(seq 1 60); do
  if ssh_node_once "$node" 'sudo -n -H -u efs /tmp/efs/efs-mgmt status 192.168.2.11:17432' > "$out/rolling-health-last.log" 2>&1; then
   if [ "$(grep -c 'Cluster state: OK (3 up, 0 down)' "$out/rolling-health-last.log")" -eq 1 ] && [ "$(grep -c ': idle' "$out/rolling-health-last.log")" -eq 3 ]; then break; fi
  fi
  if [ "$attempt" -eq 60 ]; then cat "$out/rolling-health-last.log"; exit 1; fi
  sleep 2
 done
 echo "Restarted $node"
done
for node in "$GW" "$seed"; do
 ssh_node_once "$node" 'cd /tmp/efs; sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other'
done
ssh_node_once "$GW" 'cd /tmp/efs; sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-b -o allow_other'
ssh_node_once "$GW" 'sudo -n -H -u efs python3 -u /var/tmp/efs-cold-large-verify.py /mnt/efs'
