#!/bin/bash
set -euo pipefail
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-next-three-rounds
(cd /Users/mike/git/cluster; CLUSTER=xorinox EFS_SRC=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats EFS_DIRECT_IO=1 EFS_TRANSPORT=tcp ./cluster.sh start) > "$out/final-cold-start-retry.log" 2>&1
ssh -o ControlPath=none -o ConnectTimeout=10 -i /Users/mike/git/cluster/cluster_key -J xorinox_efs efs@192.168.122.111 'cd /tmp/efs; EFS_COMMANDS=1 EFS_TRANSPORT=tcp ./scripts/client.sh 192.168.122.111:17432 /home/efs/w92-tiny-mnt'
ssh -o ControlPath=none -o ConnectTimeout=10 -i /Users/mike/git/cluster/cluster_key -J xorinox_efs efs@192.168.122.111 'python3 /home/efs/w92-next-build/tests/posix/posix_persist.py /home/efs/w92-tiny-mnt --phase verify --tag w92-next-final --keep --results /home/efs/w92-next-persist-verify.tsv' > "$out/live-persist-verify.log" 2>&1
bash "$out/cold_driver.sh" > "$out/final-cold-oracles.log" 2>&1
