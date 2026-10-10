#!/bin/bash
set -euo pipefail
cd /home/efs/w92-incremental-build
for v in 0 1; do
 cc -O3 -g -fno-omit-frame-pointer -D_GNU_SOURCE -Iinclude -Isrc/meta -Isrc/common -Ideps/blake3 /home/efs/w92-incremental-variants/frozen_catalog_bench.c /home/efs/w92-incremental-variants/r$v/query_disk.c libefs.a -lpthread -lm -ldl -libverbs -o /home/efs/w92-incremental-frozen-r$v
 strace -c -o /home/efs/w92-incremental-r$v-strace.txt /home/efs/w92-incremental-frozen-r$v /home/efs/w92-frozen-after /home/efs/w92-incremental-trace-r$v > /home/efs/w92-incremental-r$v.jsonl
done
