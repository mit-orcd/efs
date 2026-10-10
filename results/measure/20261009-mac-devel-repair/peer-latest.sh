#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261009-mac-devel-repair/latest
parent=posix2-final-9c4d
ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_2client.py --prepare /mnt/efs-h --parent $parent"
stop_client "${NODES[0]}" /mnt/efs-h
ssh_node_once "${NODES[0]}" 'cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs-h -o allow_other'
cd "$EFS_SRC"
POSIX2_STEP_SEC=1200 python3 -u tests/posix/posix_2client.py --remote mount-a mount-b --mnt /mnt/efs-h --ssh /tmp/efs-two-host-exec.py --parent "$parent" --results "$out/peer-final.tsv"
