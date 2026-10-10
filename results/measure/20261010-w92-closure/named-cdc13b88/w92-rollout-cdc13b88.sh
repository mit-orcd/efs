#!/usr/bin/env bash
set -euo pipefail
SSH=(ssh -o ControlPath=none -o ConnectTimeout=15 -i /Users/mike/git/cluster/cluster_key -J xorinox_efs)
run() { "${SSH[@]}" "$1" "set -e; $2"; }
run admin@192.168.122.115 'if sudo -n mountpoint -q /home/efs/w92-nfs-mnt; then sudo -n umount /home/efs/w92-nfs-mnt; fi'
run admin@192.168.122.114 'sudo -n systemctl stop nfs-server; sudo -n exportfs -ua'
run efs@192.168.122.114 'cd /tmp/efs; ./scripts/client.sh stop /mnt/efs'
for node in 115 116; do
  run "efs@192.168.122.$node" 'cd /tmp/efs; ./scripts/client.sh stop /mnt/efs'
done
for node in 111 112 113; do
  run "admin@192.168.122.$node" "sudo -n systemctl show efsd.service -p ExecStart --value | grep -F -- '--port 17432 --storage /data1 --quota 100G --direct-io'; sudo -n systemctl stop efsd.service"
done
# Atomic replacement leaves separate-port running daemons' executable mappings intact.
for node in 111 112 113 114 115 116; do
  run "admin@192.168.122.$node" 'sudo -n tar --exclude=results -xf /home/admin/w92-deploy-cdc13b88/w92-cdc13b88-source.tar --no-same-owner -C /tmp/efs; if sudo -n test -d /tmp/efs/results && ! sudo -n test -L /tmp/efs/results; then sudo -n mv /tmp/efs/results /tmp/efs/results.before-cdc13b88; fi; sudo -n ln -sfn /home/efs/w92-evidence-cdc13b88/results /tmp/efs/results; for binary in efsd efs-fuse efs-mgmt; do sudo -n install -o efs -g efs -m755 /home/admin/w92-deploy-cdc13b88/bins/$binary /tmp/efs/$binary.next-cdc13b88; sudo -n mv -f /tmp/efs/$binary.next-cdc13b88 /tmp/efs/$binary; done; sudo -n chown -R efs:efs /tmp/efs; sha256sum /tmp/efs/efsd /tmp/efs/efs-fuse /tmp/efs/efs-mgmt'
done
for id in 1 2 3; do
  node=$((110+id));join=''
  if [[ $id != 1 ]];then join='--join 192.168.122.111:17432';fi
  run "admin@192.168.122.$node" "sudo -n systemd-run --unit=efsd --collect --uid=efs --gid=efs -p WorkingDirectory=/tmp/efs -p StandardOutput=append:/data1/efsd.log -p StandardError=inherit -E EFS_MD_RAFT_N=3 -E EFS_TRANSPORT=tcp /tmp/efs/efsd --node-id $id --addr 192.168.122.$node --port 17432 --storage /data1 --quota 100G --direct-io $join"
done
# All three storage listeners must be recovered before client discovery.
for node in 111 112 113; do
  run "admin@192.168.122.$node" 'for attempt in $(seq 1 120); do if ss -lnt | grep -q ":17432 "; then exit 0; fi; sleep 2; done; echo "normal daemon listener recovery timeout" >&2; exit 1'
done
# Normal clients remain opted out; only acceptance mounts enable commands.
for node in 115 116; do
  run "efs@192.168.122.$node" 'cd /tmp/efs; EFS_COMMANDS=0 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /mnt/efs -o allow_other; test ! -e /mnt/efs/.efs'
done
for mount in /home/efs/w92-tiny-mnt /home/efs/w92-peer-mnt; do
  run efs@192.168.122.111 "cd /tmp/efs; EFS_COMMANDS=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 $mount"
done
run efs@192.168.122.114 'cd /tmp/efs; EFS_COMMANDS=1 EFS_FUSE_EXPORT=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /mnt/efs -o allow_other'
run admin@192.168.122.114 'sudo -n exportfs -ra; sudo -n systemctl start nfs-server; sudo -n systemctl is-active nfs-server'
