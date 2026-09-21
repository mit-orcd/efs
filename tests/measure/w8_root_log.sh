#!/bin/bash
# Last 64 KiB of each server's raft log, after the interleaved mkdir run.
# Small on purpose: 4 MiB is hours of idle traffic and hides the burst.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
mkout w8-root-log
for h in fcstor003 fcstor004 fcstor005 fcstor006; do
    say "== $h"
    ssh_ 12 "$h" 'timeout 8 python3 $HOME/git/efs/tests/tools/raft_log_tail.py /data1/01/efs/mdraft/log/raft.log 65536' \
        | tee "$OUT/$h.txt"
done
echo DONE
