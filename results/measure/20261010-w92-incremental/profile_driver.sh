#!/bin/bash
set -euo pipefail
round=$1
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-incremental
key=/Users/mike/git/cluster/cluster_key
for ip in 111 112 113; do
 ssh -i "$key" -J xorinox_efs admin@192.168.122.$ip "mkdir -p /home/admin/w92-incremental-three-rounds; (sudo -n perf record -F 99 -g -p \"\$(pgrep -x efsd)\" -o /home/admin/w92-incremental-three-rounds/round$round.data -- sleep 60; echo \$? > /home/admin/w92-incremental-three-rounds/round$round-exit) > /home/admin/w92-incremental-three-rounds/round$round-perf.log 2>&1 </dev/null &"
done
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 "python3 /home/efs/tiny_files.py /home/efs/w92-tiny-mnt/w92-incremental-round$round --max-files 6000 --max-files-per-folder 100 --max-depth 3 --max-file-size 512 --workers 4" > "$out/round$round-tiny.log" 2>&1
for ip in 111 112 113; do
 ssh -i "$key" -J xorinox_efs admin@192.168.122.$ip "for attempt in {1..40}; do test -f /home/admin/w92-incremental-three-rounds/round$round-exit && break; sleep 2; done; test \"\$(cat /home/admin/w92-incremental-three-rounds/round$round-exit)\" = 0 || exit 1; sudo -n perf report --stdio --children --call-graph none --sort symbol --percent-limit 0.5 -i /home/admin/w92-incremental-three-rounds/round$round.data" > "$out/node$ip-round$round-perf.txt"
done
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 "python3 /home/efs/tiny_oracle.py /home/efs/w92-tiny-mnt/w92-incremental-round$round /home/efs/w92-incremental-round$round-model.json capture 6000" > "$out/round$round-oracle.jsonl" 2>&1
