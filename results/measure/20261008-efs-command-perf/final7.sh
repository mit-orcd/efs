#!/bin/bash
set -euo pipefail
out=/data1/efs/command-perf/round7
# Execute only after the measured loaded round has finished.
generator=$(pgrep -f '^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-command-perf-r7$')
kill -TERM "$generator"
for i in $(seq 1 30); do kill -0 "$generator" 2>/dev/null || break; sleep 1; done
if kill -0 "$generator" 2>/dev/null; then exit 1; fi
sleep 8
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)' | paste -sd,)
perf record -F 199 -g -p "$pids" -o "$out/quiet-after.data" -- sleep 14 >"$out/quiet-after-perf.log" 2>&1 & perf_pid=$!
for i in $(seq 1 6); do /usr/bin/time -f '%e' /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json >/dev/null; done 2>"$out/quiet-after-commands.log"
wait "$perf_pid"
perf report -i "$out/quiet-after.data" --stdio --no-children --percent-limit 1 > "$out/quiet-after.txt"
python3 -m cProfile -o "$out/wrapper-after.prof" /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json > "$out/wrapper-after.json" 2>"$out/wrapper-after.stderr"
python3 - <<'PYPROFILE' > "$out/wrapper-after.txt"
import pstats
pstats.Stats('/data1/efs/command-perf/round7/wrapper-after.prof').strip_dirs().sort_stats('tottime').print_stats(25)
PYPROFILE
python3 /data1/efs/command-perf/oracle.py > "$out/production-oracle.json"
[ ! -e /data1/efs/mnt/tiny-command-perf-final7 ]
screen -S 1110701.tiny_files -X stuff $'cd /home/efs && ./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 1000000 --workers 8 /data1/efs/mnt/tiny-command-perf-final7\r'
{
 for port in 17432 17433 17434 17435; do /data1/efs/src/efs-mgmt version 127.0.0.1:$port; /data1/efs/src/efs-mgmt raft-status 127.0.0.1:$port; done
 sha256sum /data1/efs/src/efsd /data1/efs/src/efs-fuse
 for round in 1 2 3 4 5 6 7; do sha256sum /data1/efs/command-perf/round$round/rollback/efsd /data1/efs/command-perf/round$round/rollback/efs-fuse; done
 pgrep -af '^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-command-perf-final7$'
 mountpoint /data1/efs/mnt
 mountpoint /data1/efs/mnt2
 df -h /data1
 free -h
} > "$out/production-verification.txt"
