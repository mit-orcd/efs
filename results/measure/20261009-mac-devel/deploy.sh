#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-user.sh
source /Users/mike/git/cluster/lib/transfer.sh
seed=${NODES[0]}
wait_ssh "$seed"
ensure_efs_user "$seed"
ssh_node_once "$seed" 'ps -C efsd -C efs-fuse -o user,pid,args; true'
ssh_node_once "$seed" "mountpoint -q $DATA_MNT"
rsync_to "$seed" "$EFS_SRC/" /tmp/efs/ --exclude .git --exclude '*.o' --exclude '*.a' --exclude '/efsd' --exclude '/efs-fuse' --exclude '/efs-mgmt' --exclude '/efs-query' --exclude '/efs-bench'
build_id=$(git -C "$EFS_SRC" rev-parse --short=12 HEAD)
ssh_node_once "${NODES[0]}" "sudo -n chown -R efs:efs /tmp/efs && cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make -j2 -B EFS_GIT_ID=$build_id EFS_GIT_BRANCH=devel all"
ssh_node_once "$seed" "cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make EFS_GIT_ID=$build_id EFS_GIT_BRANCH=devel test"
for node in "${NODES[@]:1}" "$GW"; do
    wait_ssh "$node"
    ensure_efs_user "$node"
    if [ "$node" != "$GW" ]; then
        ssh_node_once "$node" "mountpoint -q $DATA_MNT"
        rsync_to "$node" "$EFS_SRC/" /tmp/efs/ --exclude .git --exclude '*.o' --exclude '*.a' --exclude '/efsd' --exclude '/efs-fuse' --exclude '/efs-mgmt' --exclude '/efs-query' --exclude '/efs-bench'
    fi
    ship_binaries "$seed" "$node"
done
rsync_to "$GW" "$EFS_SRC/scripts/" /tmp/efs/scripts/
rsync_to "$GW" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/
for node in "${NODES[@]}" "$GW"; do
    ssh_node_once "$node" 'cd /tmp/efs && sudo -n -H -u efs ./efsd --version && sha256sum efsd efs-fuse'
done
