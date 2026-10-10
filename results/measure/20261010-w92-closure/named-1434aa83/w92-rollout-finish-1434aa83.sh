#!/usr/bin/env bash
set -euo pipefail
SSH=(ssh -o ControlPath=none -o ConnectTimeout=15 -i /Users/mike/git/cluster/cluster_key -J xorinox_efs)
run() { "${SSH[@]}" "$1" "set -e; $2"; }
# Normal clients remain opted out; only acceptance mounts enable commands.
for node in 115 116; do
  run "efs@192.168.122.$node" 'cd /tmp/efs; EFS_COMMANDS=0 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /mnt/efs -o allow_other; test ! -e /mnt/efs/.efs'
done
for mount in /home/efs/w92-tiny-mnt /home/efs/w92-peer-mnt; do
  run efs@192.168.122.111 "cd /tmp/efs; EFS_COMMANDS=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 $mount"
done
run efs@192.168.122.114 'cd /tmp/efs; EFS_COMMANDS=1 EFS_FUSE_EXPORT=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /mnt/efs -o allow_other'
run admin@192.168.122.114 'sudo -n exportfs -ra; sudo -n systemctl start nfs-server; sudo -n systemctl is-active nfs-server'
