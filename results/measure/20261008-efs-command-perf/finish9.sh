#!/bin/bash
set -euo pipefail
out=/data1/efs/command-perf/round9
python3 /data1/efs/command-perf/direct-rpc.py "$out" > "$out/direct-rpc.log"
python3 /data1/efs/command-perf/oracle.py > "$out/production-oracle.json"
[ ! -e /data1/efs/mnt/tiny-command-perf-final9 ]
screen -S 1110701.tiny_files -X stuff $'cd /home/efs && ./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 1000000 --workers 8 /data1/efs/mnt/tiny-command-perf-final9\r'
{
 for port in 17432 17433 17434 17435; do /data1/efs/src/efs-mgmt version 127.0.0.1:$port; /data1/efs/src/efs-mgmt raft-status 127.0.0.1:$port; done
 sha256sum /data1/efs/src/efsd /data1/efs/src/efs-fuse
 for round in 8 9; do sha256sum /data1/efs/command-perf/round$round/rollback/efsd /data1/efs/command-perf/round$round/rollback/efs-fuse; done
 pgrep -af '^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-command-perf-final9$'
 mountpoint /data1/efs/mnt
 mountpoint /data1/efs/mnt2
 df -h /data1 /data2/efs/n3
 df -i /data1
 free -h
} > "$out/production-verification.txt"
