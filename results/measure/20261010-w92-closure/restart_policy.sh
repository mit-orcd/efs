#!/bin/bash
set -euo pipefail
mode=${1:?disabled enabled or quota}
case "$mode" in
 disabled) extra='-E EFS_COMMANDS_SERVER=0';;
 enabled) extra='-E EFS_COMMANDS_SERVER=1';;
 quota) extra='-E EFS_COMMANDS_SERVER=1 -E EFS_QUERY_DISK_TOTAL_MIB=1';;
 *) exit 2;;
esac
export PATH=/private/tmp/w92-ssh-bin:$PATH
key=/Users/mike/git/cluster/cluster_key
for mnt in /home/efs/w92-peer-mnt /home/efs/w92-tiny-mnt; do
 ssh -o ControlPath=none -i "$key" -J xorinox_efs efs@192.168.122.111 "cd /tmp/efs; ./scripts/client.sh stop $mnt"
done
(cd /Users/mike/git/cluster; CLUSTER=xorinox EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp ./cluster.sh stop)
for id in 1 2 3; do
 ip=192.168.122.$((110+id))
 ssh -o ControlPath=none -i "$key" -J xorinox_efs efs@"$ip" "wc -l < /data1/efsd.log > /home/efs/w92-$mode-log-start"
 join=''
 if [ "$id" != 1 ]; then join='--join 192.168.122.111:17432'; fi
 ssh -o ControlPath=none -i "$key" -J xorinox_efs admin@"$ip" "set -eu; mountpoint -q /data1; test \"\$(findmnt -R /data1 -n -o TARGET | wc -l)\" -eq 1; test \"\$(stat -c %U:%G /data1)\" = efs:efs; sudo -n -u efs touch /data1/efsd.log; sudo -n systemd-run --unit=efsd --collect --uid=efs --gid=efs -p WorkingDirectory=/tmp/efs -p StandardOutput=append:/data1/efsd.log -p StandardError=inherit -E EFS_MD_RAFT_N=3 -E EFS_TRANSPORT=tcp $extra /tmp/efs/efsd --node-id $id --addr $ip --port 17432 --storage /data1 --quota 100G --direct-io $join"
done
(cd /Users/mike/git/cluster; CLUSTER=xorinox EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp ./cluster.sh start)
if [ "$mode" != disabled ]; then
 for mnt in /home/efs/w92-tiny-mnt /home/efs/w92-peer-mnt; do
  ssh -o ControlPath=none -i "$key" -J xorinox_efs efs@192.168.122.111 "cd /tmp/efs; EFS_COMMANDS=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 $mnt"
 done
fi
