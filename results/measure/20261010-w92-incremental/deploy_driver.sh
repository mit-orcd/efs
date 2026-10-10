#!/bin/bash
set -e
# Prefer the existing cluster key over stale helper password authentication.
export PATH=/private/tmp/w92-ssh-bin:$PATH
ssh -i /Users/mike/git/cluster/cluster_key -J xorinox_efs efs@192.168.122.111 'cd /tmp/efs; ./scripts/client.sh stop /home/efs/w92-tiny-mnt'
cd /Users/mike/git/cluster
export CLUSTER=xorinox EFS_SRC=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp
./cluster.sh stop
ssh -i /Users/mike/git/cluster/cluster_key -J xorinox_efs admin@192.168.122.111 'for b in efsd efs-fuse efs-mgmt efs-query efs-bench; do sudo -n install -m755 /home/efs/w92-incremental-build/$b /tmp/efs/$b; done'
bash -c 'source ./env.sh; SSH_OPTS="-o ControlMaster=no -o ControlPath=none -o ControlPersist=no $SSH_OPTS"; source ./lib/transfer.sh; for n in "${NODES[@]:1}" "$GW"; do ship_binaries "${NODES[0]}" "$n" || exit 1; done'
./cluster.sh start
ssh -i /Users/mike/git/cluster/cluster_key -J xorinox_efs efs@192.168.122.111 'cd /tmp/efs; EFS_COMMANDS=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /home/efs/w92-tiny-mnt'
