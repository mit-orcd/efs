#!/bin/bash
set -eo pipefail
export CLUSTER=mac EFS_SRC=/private/tmp/efs-integration-20261009
source /Users/mike/git/cluster/env.sh
source /Users/mike/git/cluster/lib/client-stop.sh
out=$EFS_SRC/results/measure/20261010-mac-integration-2/final-batch
parent=posix2-integration2-finalbatch-20261010
ssh_node_once "$GW" "sudo -n -H -u efs python3 -u /tmp/efs/tests/posix/posix_2client.py --prepare /mnt/efs --parent $parent"
stop_client "${NODES[0]}" /mnt/efs
ssh_node_once "${NODES[0]}" 'cd /tmp/efs && sudo -n -H -u efs env EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.2.11:17432 /mnt/efs -o allow_other'
cd /private/tmp/efs-mac-final2-harness
POSIX2_STEP_SEC=2400 python3 -u tests/posix/posix_2client.py --remote mount-a mount-b --mnt /mnt/efs --ssh /tmp/efs-two-host-final2-exec.py --parent "$parent" --results "$out/peer-final.tsv"

printf "peer\t0\n" >> "$out/final-verdicts.tsv"
