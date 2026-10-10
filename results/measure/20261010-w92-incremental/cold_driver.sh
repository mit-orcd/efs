#!/bin/bash
set -euo pipefail
ssh -i /Users/mike/git/cluster/cluster_key -J xorinox_efs efs@192.168.122.111 'set -e
for spec in w92-tiny-baseline:12000 w92-tiny-after:12000 w92-round1:6000 w92-round2:6000 w92-round3:6000 w92-merged-round1:6000 w92-merged-round2:6000 w92-merged-round3:6000 w92-next-round1:6000 w92-next-round2:6000 w92-next-round3:6000 w92-incremental-round1:6000 w92-incremental-round2:6000 w92-incremental-round3:6000; do
 name=${spec%:*}; count=${spec#*:}
 printf "%s " "$name"
 python3 /home/efs/tiny_oracle.py /home/efs/w92-tiny-mnt/$name /home/efs/$name-model.json check "$count"
done
python3 /home/efs/w92-tiny-mnt/.efs/stats --json'
