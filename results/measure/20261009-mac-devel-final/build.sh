#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
seed=${NODES[0]}
build_id=$(git -C "$EFS_SRC" rev-parse --short=12 HEAD)
stage=/var/tmp/efs-devel-$build_id
ssh_node_once "$seed" "sudo -n -H -u efs mkdir -p $stage"
rsync_to "$seed" "$EFS_SRC/" "$stage/" --rsync-path='sudo -n -u efs rsync' --exclude .git --exclude '*.o' --exclude '*.a' --exclude '/efsd' --exclude '/efs-fuse' --exclude '/efs-mgmt' --exclude '/efs-query' --exclude '/efs-bench'
ssh_node_once "$seed" "cd $stage && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make -j2 EFS_GIT_ID=$build_id EFS_GIT_DESCRIBE=$build_id EFS_GIT_BRANCH=devel all && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make EFS_GIT_ID=$build_id EFS_GIT_DESCRIBE=$build_id EFS_GIT_BRANCH=devel test"
