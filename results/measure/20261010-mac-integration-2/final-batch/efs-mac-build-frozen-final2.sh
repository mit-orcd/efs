#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
seed=${NODES[0]}
build_id=03db5552cc2d
stage=/var/tmp/efs-devel-$build_id
source_archive=/private/tmp/efs-build-source-$build_id.tar.gz
git -C "$EFS_SRC" archive "$build_id" | gzip -1 > "$source_archive"
rsync_to "$seed" "$source_archive" /var/tmp/efs-build-source-$build_id.tar.gz
ssh_node_once "$seed" "sudo -n -H -u efs mkdir -p $stage && sudo -n -H -u efs tar -xzf /var/tmp/efs-build-source-$build_id.tar.gz -C $stage"
ssh_node_once "$seed" "cd $stage && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make -j2 EFS_GIT_ID=$build_id EFS_GIT_DESCRIBE=$build_id EFS_GIT_BRANCH=devel all && sudo -n -H -u efs env EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_ADMISSION_PRESSURE=0 make EFS_GIT_ID=$build_id EFS_GIT_DESCRIBE=$build_id EFS_GIT_BRANCH=devel test"
