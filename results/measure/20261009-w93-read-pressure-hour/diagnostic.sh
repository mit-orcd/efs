#!/bin/bash
set -euo pipefail
base=/data1/efs/w93-read-pressure-hour-20261009
cd "$base"
mkdir perf-read strace-read
ids=$(pgrep -f '^/data1/efs/src/(efsd|efs-fuse) ' | paste -sd,)
ps -p "${ids//,/ }" -o pid,args > profile-processes.txt || true
python3 read-capture.py perf-read 30 > perf-read/read.log.tmp 2>&1 &
reader=$!
perf record -e cpu-clock -F 99 -g --call-graph dwarf,8192 -p "$ids" -o read-perf.data -- sleep 30 > perf-record.log 2>&1
wait "$reader"
perf report -i read-perf.data --stdio --no-children --sort pid,dso,symbol --percent-limit 0.5 > perf-report.txt 2>&1
sha256sum read-perf.data > perf-sha256.txt
n3=$(pgrep -f '^/data1/efs/src/efsd --node-id 3 ')
n4=$(pgrep -f '^/data1/efs/src/efsd --node-id 4 ')
python3 read-capture.py strace-read 20 > strace-read/read.log.tmp 2>&1 &
reader=$!
set +e
timeout --signal=INT 12 strace -f -c -w -e trace=fdatasync,fsync,pwrite64,pread64,futex,poll,ppoll,epoll_wait -p "$n3" -p "$n4" -o strace-summary.txt > strace-controller.log 2>&1
printf '%s\n' "$?" > strace-exit.txt
set -e
wait "$reader"
