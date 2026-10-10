#!/bin/bash
set -euo pipefail
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-next-three-rounds
key=/Users/mike/git/cluster/cluster_key
rsync -e "ssh -i $key -J xorinox_efs" "$out/repeat_frozen.py" efs@192.168.122.111:/home/efs/repeat_next.py
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'python3 /home/efs/repeat_next.py /home/efs /home/efs/w92-frozen-after' > "$out/repeat-frozen.jsonl" 2>&1
rsync -a -e "ssh -i $key -J xorinox_efs" --exclude=.git --exclude=results --exclude='*.o' --exclude='*.a' --exclude=.build_id.stamp --exclude=/efsd --exclude=/efs-fuse --exclude=/efs-mgmt --exclude=/efs-query --exclude=/efs-bench ./ efs@192.168.122.111:/home/efs/w92-next-build/
rsync -a -e "ssh -i $key -J xorinox_efs" "$out/" efs@192.168.122.111:/home/efs/w92-next-build/results/measure/20261010-w92-next-three-rounds/
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'cd /home/efs/w92-next-build && make test EFS_GIT_ID=f5164bb3 EFS_GIT_DESCRIBE=f5164bb3 EFS_GIT_BRANCH=devel-find-stats' > "$out/final-native-gates.log" 2>&1
(cd /Users/mike/git/cluster; CLUSTER=xorinox EFS_SRC=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats ./posix.sh all) > "$out/live-posix.log" 2>&1
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'python3 /home/efs/w92-next-build/tests/posix/posix_extras.py /home/efs/w92-tiny-mnt --results /home/efs/w92-next-extras.tsv' > "$out/live-extras.log" 2>&1
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'python3 /home/efs/w92-next-build/tests/posix/posix_persist.py /home/efs/w92-tiny-mnt --phase prepare --tag w92-next-final --keep --results /home/efs/w92-next-persist-prepare.tsv' > "$out/live-persist-prepare.log" 2>&1
bash "$out/deploy_driver.sh" > "$out/final-cold-restart.log" 2>&1
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'python3 /home/efs/w92-next-build/tests/posix/posix_persist.py /home/efs/w92-tiny-mnt --phase verify --tag w92-next-final --keep --results /home/efs/w92-next-persist-verify.tsv' > "$out/live-persist-verify.log" 2>&1
bash "$out/cold_driver.sh" > "$out/final-cold-oracles.log" 2>&1
