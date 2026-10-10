#!/bin/bash
# Temporary source-engine capacity; caller must finish native tests first.
set -euo pipefail
export PATH=/private/tmp/w92-ssh-bin:$PATH
key=/Users/mike/git/cluster/cluster_key
ssh -o ControlPath=none -i "$key" -J xorinox_efs efs@192.168.122.111 'test -z "$(pgrep -f "^/home/efs/w92-query-scale-sequential " || true)"'
for spec in '111 /home/efs/w92-tiny-mnt' '111 /home/efs/w92-peer-mnt' '112 /home/efs/w92-survivor-mnt'; do
 read -r ip mnt <<< "$spec"
 ssh -o ControlPath=none -i "$key" -J xorinox_efs efs@192.168.122."$ip" "cd /tmp/efs; ./scripts/client.sh stop $mnt"
done
(cd /Users/mike/git/cluster; CLUSTER=xorinox EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp ./cluster.sh stop)
ssh -o ControlPath=none xorinox_efs 'set -eu
 test "$(virsh dominfo xefs1 | awk "/Max memory:/ {print \$3}")" = 3145728
 test "$(awk "/MemAvailable:/ {print \$2}" /proc/meminfo)" -gt 25165824
 test ! -e /data1/efs/bench/w92-xefs1-original-20261010.xml
 virsh dumpxml --inactive xefs1 > /data1/efs/bench/w92-xefs1-original-20261010.xml
 virsh shutdown xefs1'
