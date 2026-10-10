#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/transfer.sh
source /Users/mike/git/cluster/lib/client-user.sh
seed=${NODES[0]}
stage=/var/tmp/efs-devel-9c4db072de1a
ssh_node_once "$seed" "set -e; next=\$(mktemp -d /tmp/efs/.next.XXXXXX); for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do cp $stage/\$b \$next/\$b; mv -f \$next/\$b /tmp/efs/\$b; done; rmdir \$next"
for node in "${NODES[@]:1}" "$GW"; do ship_binaries "$seed" "$node"; done
for node in "${NODES[@]}"; do
    echo "Rolling restart $node; retained gateway remains mounted"
    ssh_node_once "$node" 'sudo -n systemctl restart efsd'
    for attempt in $(seq 1 60); do
        if ssh_node_once "$node" 'sudo -n -H -u efs /tmp/efs/efs-mgmt status 192.168.2.11:17432' > /tmp/efs-mac-rolling-health.txt 2>&1; then
            if [ "$(grep -c 'Cluster state: OK (3 up, 0 down)' /tmp/efs-mac-rolling-health.txt)" -eq 1 ] && [ "$(grep -c ': idle' /tmp/efs-mac-rolling-health.txt)" -eq 3 ]; then break; fi
        fi
        if [ "$attempt" -eq 60 ]; then cat /tmp/efs-mac-rolling-health.txt; exit 1; fi
        sleep 2
    done
    cat /tmp/efs-mac-rolling-health.txt
    ssh_node_once "$node" 'ps -C efsd -o user,pid,args; sudo -n -H -u efs /tmp/efs/efsd --version'
done
rsync_to "$GW" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/
ssh_node_once "$GW" 'set -e; ! mountpoint -q /mnt/efs-h; sudo -n install -d -m 0755 -o efs -g efs /mnt/efs-h; sudo -n touch /mnt/.efs-efs-h.lock /mnt/efs-fuse-efs-h.log; sudo -n chown efs:efs /mnt/.efs-efs-h.lock /mnt/efs-fuse-efs-h.log'
ssh_node_once "$GW" 'cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-h -o allow_other'
rsync_to "$GW" /tmp/efs-cold-large-verify.py /var/tmp/efs-cold-large-verify.py
ssh_node_once "$GW" 'sudo -n -H -u efs python3 -u /var/tmp/efs-cold-large-verify.py /mnt/efs-h'
