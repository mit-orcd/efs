#!/bin/bash
set -euo pipefail
round=$1
out=/data1/efs/command-perf/round$round
mkdir -p "$out"
generator=$(pgrep -f "^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-command-perf-r$round$")
kill -TERM "$generator"
for i in $(seq 1 30); do kill -0 "$generator" 2>/dev/null || break; sleep 1; done
if kill -0 "$generator" 2>/dev/null; then exit 1; fi
sleep 8
python3 /data1/efs/command-perf/hardware.py "$out" > "$out/hardware-summary.log"
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)' | paste -sd,)
perf record -F 199 -g -p "$pids" -o "$out/quiet-after.data" -- sleep 15 >"$out/quiet-perf.log" 2>&1 & perf_pid=$!
for i in $(seq 1 16); do /usr/bin/time -f '%e' /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json >/dev/null; done 2>"$out/quiet-commands.log"
wait "$perf_pid"
perf report -i "$out/quiet-after.data" --stdio --no-children --percent-limit 1 > "$out/quiet-after.txt"
pid=$(pgrep -f '^/data1/efs/src/efsd .*--port 17433( |$)')
timeout -s TERM 8 strace -f -c -p "$pid" -o "$out/server-after.strace" >"$out/server-strace.log" 2>&1 & trace_pid=$!
for i in $(seq 1 12); do /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json >/dev/null 2>/dev/null; done
wait "$trace_pid" || [ "$?" = 124 ]
