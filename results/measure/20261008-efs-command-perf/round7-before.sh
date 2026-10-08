#!/bin/bash
set -euo pipefail
out=/data1/efs/command-perf/round7
mkdir -p "$out"
generator=$(pgrep -f '^python3 ./tiny_files.py .* /data1/efs/mnt/tiny-command-perf-r6$')
kill -TERM "$generator"
for i in $(seq 1 30); do kill -0 "$generator" 2>/dev/null || break; sleep 1; done
if kill -0 "$generator" 2>/dev/null; then exit 1; fi
sleep 8
pids=$(pgrep -f '^/data1/efs/src/efsd .*--port 1743[2-5]( |$)' | paste -sd,)
perf record -F 199 -g -p "$pids" -o "$out/quiet-before.data" -- sleep 14 >"$out/quiet-before-perf.log" 2>&1 & perf_pid=$!
for i in $(seq 1 6); do /usr/bin/time -f '%e' /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json >/dev/null; done 2>"$out/quiet-before-commands.log"
wait "$perf_pid"
perf report -i "$out/quiet-before.data" --stdio --no-children --percent-limit 1 > "$out/quiet-before.txt"
python3 -m cProfile -o "$out/wrapper-before.prof" /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json > "$out/wrapper-before.json" 2>"$out/wrapper-before.stderr"
python3 - <<'PY' > "$out/wrapper-before.txt"
import pstats
pstats.Stats('/data1/efs/command-perf/round7/wrapper-before.prof').strip_dirs().sort_stats('tottime').print_stats(25)
PY
strace -f -tt -T -o "$out/find-before.strace" /data1/efs/mnt/tiny-files4/.efs/find file --match prefix --json >/dev/null 2>"$out/find-before.stderr"
