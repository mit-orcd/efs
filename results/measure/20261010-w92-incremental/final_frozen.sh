#!/bin/bash
set -euo pipefail
key=/Users/mike/git/cluster/cluster_key
out=/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-find-stats/results/measure/20261010-w92-incremental
for v in 2 3; do
 ssh -i "$key" -J xorinox_efs efs@192.168.122.111 "mkdir -p /home/efs/w92-incremental-variants/r$v"
 rsync -a -e "ssh -i $key -J xorinox_efs" "$out/variant$v/" efs@192.168.122.111:/home/efs/w92-incremental-variants/r$v/
done
rsync -a -e "ssh -i $key -J xorinox_efs" "$out/frozen_catalog_bench.c" "$out/repeat_frozen.py" efs@192.168.122.111:/home/efs/w92-incremental-variants/
ssh -i "$key" -J xorinox_efs efs@192.168.122.111 'bash -s' <<'REMOTE'
set -euo pipefail
cd /home/efs/w92-incremental-build
for v in 0 1 2 3; do
 cc -O3 -g -fno-omit-frame-pointer -D_GNU_SOURCE -Iinclude -Isrc/meta -Isrc/common -Ideps/blake3 /home/efs/w92-incremental-variants/frozen_catalog_bench.c /home/efs/w92-incremental-variants/r$v/query_disk.c libefs.a -lpthread -lm -ldl -libverbs -o /home/efs/w92-incremental-frozen-r$v
done
for v in 2 3; do
 strace -c -o /home/efs/w92-incremental-r$v-strace.txt /home/efs/w92-incremental-frozen-r$v /home/efs/w92-frozen-after /home/efs/w92-incremental-trace-r$v > /home/efs/w92-incremental-r$v.jsonl
done
python3 /home/efs/w92-incremental-variants/repeat_frozen.py /home/efs /home/efs/w92-frozen-after > /home/efs/w92-incremental-repeat-frozen.jsonl
REMOTE
rsync -a -e "ssh -i $key -J xorinox_efs" 'efs@192.168.122.111:/home/efs/w92-incremental-r[23]*.txt' 'efs@192.168.122.111:/home/efs/w92-incremental-r[23].jsonl' efs@192.168.122.111:/home/efs/w92-incremental-repeat-frozen.jsonl "$out/"
