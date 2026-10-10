#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-checkpoint-hour-20261010
cd "$base"
python3 balanced-capture.py profiled 90 10 reuse > profiled-driver.log 2>&1 &
capture=$!
completed=0
cleanup() {
 if [ "$completed" = 0 ] && [ -f profiled/loader.pid ]; then
  loader=$(cat profiled/loader.pid)
  kill -USR1 "$loader" 2>/dev/null || true
  wait "$capture" 2>/dev/null || true
 fi
}
trap cleanup EXIT
for i in $(seq 1 300); do
 [ -f profiled/load-start.json ] && break
 kill -0 "$capture" 2>/dev/null || { wait "$capture"; exit 1; }
 sleep .2
done
[ -f profiled/load-start.json ] || { echo 'profile preparation deadline';exit 1; }
ids=$( (pgrep -x efsd; pgrep -x efs-fuse) | paste -sd,)
ps -p "${ids//,/ }" -o pid,args > profiled-processes.txt
perf record -e cpu-clock -F 99 -g --call-graph dwarf,8192 -p "$ids" -o checkpoint-perf.data -- sleep 30 > perf-record.log 2>&1
perf report -i checkpoint-perf.data --stdio --no-children --sort pid,dso,symbol --percent-limit .5 > perf-report.txt 2>&1
perf report -i checkpoint-perf.data --stdio --children --sort symbol --percent-limit 1 > perf-inclusive.txt 2>&1
sha256sum checkpoint-perf.data > perf-sha256.txt
n3=$(pgrep -f '^/data1/efs/src/efsd --node-id 3 ')
n4=$(pgrep -f '^/data1/efs/src/efsd --node-id 4 ')
set +e
timeout --signal=INT 12 strace -f -c -w -e trace=fdatasync,fsync,pwrite64,pread64,futex,poll,ppoll,epoll_wait -p "$n3" -p "$n4" -o strace-summary.txt > strace-controller.log 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > strace-exit.txt
[ "$rc" = 124 ] || [ "$rc" = 0 ]
set +e
timeout --signal=INT 8 strace -ff -tt -T -yy -e trace=fdatasync,pwrite64 -p "$n3" -p "$n4" -o strace-path > strace-path-controller.log 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > strace-path-exit.txt
[ "$rc" = 124 ] || [ "$rc" = 0 ]
wait "$capture"
completed=1
printf 'profiled workload is diagnostic only\n' > profile-pass.txt
