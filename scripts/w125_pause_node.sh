#!/bin/bash
# w125_pause_node.sh REL OUT [PORT] [PAUSE_S]
#
# Interruptions under load on a private node9901 cluster (w125_p0_cluster.sh,
# ports PORT+1..PORT+3, default 17711-17713; never orcd-mini or w100).
# w125_concurrent.py runs 4 writers, 4 verifying readers, 2 deleters for
# 270 s (packing at minimum age 2 s, pack-compact every 30 s). Daemons are
# frozen with SIGSTOP for PAUSE_S (default 15) seconds and resumed with
# SIGCONT, which peers see like a network partition of that node:
#   t=60 s   the group-0 metadata leader;
#   t=150 s  a node that leads no group (a follower and storage member).
# Every error the client returned is listed (client log lines with an
# efs_rc, and the tool's ERR lines). Exit 1 on any mismatch or client error.
set -u
REL=$1; OUT=$2; PORT=${3:-17710}; PAUSE=${4:-15}
HERE=$(cd "$(dirname "$0")" && pwd); RUN=/tmp/w125-pause-run; M=$REL/efs-mgmt
mkdir -p "$OUT"
export EFS_PACK_MIN_AGE_MS=2000 LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
"$HERE/w125_p0_cluster.sh" start "$REL" $RUN $PORT > "$OUT/start.txt" 2>&1 || { cat "$OUT/start.txt"; exit 1; }
lead_of() { for i in 1 2 3; do $M raft-status 10.1.99.1:$((PORT + i)) 2>/dev/null | grep -q "^  group $1 .*role=LEADER" && echo $i; done | head -1; }
pause() { # node label
  local pid; pid=$(cat $RUN/logs/n$1.pid)
  echo "$(date -u +%T) SIGSTOP n$1 ($2) for ${PAUSE}s" >> "$OUT/events.txt"
  kill -STOP $pid; sleep "$PAUSE"; kill -CONT $pid
  echo "$(date -u +%T) SIGCONT n$1" >> "$OUT/events.txt"
}
python3 "$HERE/w125_concurrent.py" $RUN/mnt/c 270 --writers 4 --readers 4 --deleters 2 > "$OUT/conc.txt" 2>&1 &
cp=$!
( for t in $(seq 9); do sleep 30; $M pack-compact 10.1.99.1:$((PORT + 1)) 64 > /dev/null 2>&1; done ) &
sleep 60; l=$(lead_of 0); pause "${l:-1}" "group-0 leader"
sleep 75; f=""; for i in 1 2 3; do [ "$i" != "$(lead_of 0)" ] && [ "$i" != "$(lead_of 2)" ] && f=$i; done
pause "${f:-3}" "leads no group"
wait $cp; rc=$?
grep -a "efs_rc=" $RUN/logs/fuse.log | grep -v "report-split" > "$OUT/client-errors.txt"
grep -h "became\|term=\|unresponsive" $RUN/logs/efsd-n*.log | grep -v "packer\|apply-sleep" | tail -n 20 > "$OUT/raft.txt"
"$HERE/w125_p0_cluster.sh" stop "$REL" $RUN $PORT > /dev/null 2>&1; rm -rf $RUN
cat "$OUT/events.txt"; grep -E '^final|^ERR' "$OUT/conc.txt" | head -12
echo "client_error_lines=$(grep -c . "$OUT/client-errors.txt")"
sed -E 's/^[^ ]+ //; s/ino=[0-9]+.*//' "$OUT/client-errors.txt" | sort | uniq -c | sort -rn | head
[ $rc -eq 0 ] && [ ! -s "$OUT/client-errors.txt" ]
