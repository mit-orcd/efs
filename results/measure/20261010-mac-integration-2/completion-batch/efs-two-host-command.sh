#!/bin/bash
set -eo pipefail
peer_step_budget=${EFS_SSH_TIMEOUT:-180}
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh >/dev/null
case "$1" in mount-a) peer_node=$GW;; mount-b) peer_node=${NODES[0]};; *) exit 2;; esac
printf -v peer_command '%q' "$2"
ssh_node_once "$peer_node" "sudo -n -H -u efs timeout --kill-after=5s ${peer_step_budget}s bash -c $peer_command"
