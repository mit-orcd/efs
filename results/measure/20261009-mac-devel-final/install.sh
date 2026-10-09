#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-user.sh
source /Users/mike/git/cluster/lib/transfer.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-final
build_id=$(cut -c1-12 "$out/source-commit.txt")
stage=/var/tmp/efs-devel-$build_id
/Users/mike/git/cluster/cluster.sh stop > "$out/stop-before-deploy.log" 2>&1
seed=${NODES[0]}
ssh_node_once "$seed" "sudo -n chown -R admin:admin /tmp/efs"
rsync_to "$seed" "$EFS_SRC/" /tmp/efs/ --exclude .git --exclude '*.o' --exclude '*.a' --exclude '/efsd' --exclude '/efs-fuse' --exclude '/efs-mgmt' --exclude '/efs-query' --exclude '/efs-bench'
ssh_node_once "$seed" "for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do sudo -n install -o efs -g efs -m 755 $stage/\$b /tmp/efs/\$b; done"
for node in "${NODES[@]:1}" "$GW"; do
    if [ "$node" != "$GW" ]; then
        rsync_to "$node" "$EFS_SRC/" /tmp/efs/ --exclude .git --exclude '*.o' --exclude '*.a' --exclude '/efsd' --exclude '/efs-fuse' --exclude '/efs-mgmt' --exclude '/efs-query' --exclude '/efs-bench'
    fi
    ship_binaries "$seed" "$node"
done
rsync_to "$GW" "$EFS_SRC/scripts/" /tmp/efs/scripts/
rsync_to "$GW" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/
for node in "${NODES[@]}" "$GW"; do
    ssh_node_once "$node" 'cd /tmp/efs; sudo -n -H -u efs ./efsd --version; sha256sum efsd efs-fuse'
done
