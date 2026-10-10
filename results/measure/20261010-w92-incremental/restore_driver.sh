#!/bin/bash
set -euo pipefail
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-incremental
key=/Users/mike/git/cluster/cluster_key
ssh -o ControlPath=none -o ConnectTimeout=10 -i "$key" -J xorinox_efs efs@192.168.122.111 'cd /tmp/efs; ./scripts/client.sh stop /home/efs/w92-tiny-mnt; ! mountpoint -q /home/efs/w92-tiny-mnt'
for ip in 114 115 116; do
 ssh -o ControlPath=none -o ConnectTimeout=10 -i "$key" -J xorinox_efs efs@192.168.122.$ip 'findmnt -t fuse.efs-fuse /mnt/efs; test ! -e /mnt/efs/.efs; printf "ordinary .efs disabled: PASS\n"'
done
for ip in 111 112 113 114 115 116; do
 rsync -a -e "ssh -o ControlPath=none -o ConnectTimeout=10 -i $key -J xorinox_efs" --exclude=.git --exclude=results --exclude='*.o' --exclude='*.a' --exclude=.build_id.stamp --exclude=/efsd --exclude=/efs-fuse --exclude=/efs-mgmt --exclude=/efs-query --exclude=/efs-bench ./ admin@192.168.122.$ip:/tmp/efs/
 rsync -a -e "ssh -o ControlPath=none -o ConnectTimeout=10 -i $key -J xorinox_efs" "$out/" admin@192.168.122.$ip:/tmp/efs/results/measure/20261010-w92-incremental/
done
