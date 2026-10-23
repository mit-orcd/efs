#!/bin/bash
# intel_offcpu_profile.sh OUT [CASES...]
# Off-CPU profile of the Intel cluster (efsd on intel1/intel2) and its
# xorinox client (efs-fuse on /data1/efs/intel-mnt) under one workload per
# case. Cases: tiny8 tiny16 tiny32 write read (default: all). Each case runs
# its workload on xorinox, waits WARM s, then records CAP s of sched_switch
# with DWARF stacks, filtered to efsd/efs-fuse switches, on all three hosts.
# Raw perf.data stays on each host under RAW; scripts/offcpu_summary.py
# writes summary.txt next to it, and those summaries are copied to OUT.
# Prerequisites: kernel.perf_event_paranoid=-1, the cluster and the
# xorinox mount running, tiny_files.py at /data1/efs/intel-client/.
set -u
OUT=$1; shift; CASES=${*:-tiny8 tiny16 tiny32 write read}
WARM=${WARM:-15}; CAP=${CAP:-8}; RUNSEC=${RUNSEC:-45}
TAG=${TAG:-offcpu-$(date -u +%Y%m%d-%H%M)}
RAWS=/data1/efs/prof/$TAG; RAWC=/data1/efs/intel-client/prof/$TAG
MNT=/data1/efs/intel-mnt; FIX=/data1/efs/intel-client/prof-fixture-2g.bin
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; log(){ echo "$(date -u +%FT%TZ) $*" | tee -a "$OUT/run.log"; }
for h in intel1_efs intel2_efs xorinox_efs; do scp -q "$HERE/offcpu_summary.py" $h:/tmp/offcpu_summary.py; done
cpid=$(ssh xorinox_efs 'for p in $(pgrep -u efs -x efs-fuse); do tr "\0" "\n" < /proc/$p/cmdline | grep -qx /data1/efs/intel-mnt && echo $p; done')
p1=$(ssh intel1_efs 'pgrep -u efs -x efsd | paste -sd" "'); p2=$(ssh intel2_efs 'pgrep -u efs -x efsd | paste -sd" "')
log "tag=$TAG client=$cpid intel1=[$p1] intel2=[$p2] warm=${WARM}s cap=${CAP}s"
cap(){ # host dir comm
  ssh $1 "mkdir -p $2 && perf record -a -e sched:sched_switch --filter 'prev_comm == \"$3\" || next_comm == \"$3\"' --call-graph dwarf,8192 -o $2/off.data -- sleep $CAP > $2/record.log 2>&1"
}
summ(){ # host dir pids
  args=$(for p in $3; do printf -- '--pid %s ' $p; done)
  ssh $1 "cd $2 && perf script -i off.data -F pid,tid,time,event,trace,ip,sym 2>/dev/null | python3 /tmp/offcpu_summary.py $args --top 30 --frames 8 > summary.txt; sha256sum off.data; du -h off.data | cut -f1"
}
for c in $CASES; do
  case $c in
    tiny*) w=${c#tiny}; wl="d=$MNT/off-$c-\$\$; timeout -s KILL $RUNSEC python3 /data1/efs/intel-client/tiny_files.py \$d --max-files 5000000 --workers $w --max-depth 2 >/dev/null 2>&1; echo files=\$(find \$d -type f | wc -l)" ;;
    write) wl="mkdir -p $MNT/off-w; s=\$(date +%s.%N); for i in \$(seq 1 8); do dd if=$FIX of=$MNT/off-w/w\$i bs=1M conv=fsync status=none & done; wait; echo secs=\$(python3 -c \"import time;print(round(time.time()-\$s,1))\") bytes=17179869184" ;;
    read) wl="s=\$(date +%s.%N); for i in \$(seq 1 8); do dd if=$MNT/off-w/w\$i of=/dev/null bs=1M status=none & done; wait; echo secs=\$(python3 -c \"import time;print(round(time.time()-\$s,1))\") bytes=17179869184" ;;
  esac
  log "$c start"
  ssh xorinox_efs "$wl" > "$OUT/$c.result" 2>&1 &
  wpid=$!; sleep "$WARM"
  cap intel1_efs $RAWS/$c efsd & cap intel2_efs $RAWS/$c efsd & cap xorinox_efs $RAWC/$c efs-fuse & wait
  log "$c captured"; wait $wpid; log "$c done: $(cat "$OUT/$c.result")"
done
for c in $CASES; do
  ( echo "$c intel1 $(summ intel1_efs $RAWS/$c "$p1" | tr '\n' ' ')"
    echo "$c intel2 $(summ intel2_efs $RAWS/$c "$p2" | tr '\n' ' ')"
    echo "$c xorinox $(summ xorinox_efs $RAWC/$c "$cpid" | tr '\n' ' ')" ) >> "$OUT/hashes.txt" &
done; wait
for c in $CASES; do
  scp -q intel1_efs:$RAWS/$c/summary.txt "$OUT/$c-intel1.txt"; scp -q intel2_efs:$RAWS/$c/summary.txt "$OUT/$c-intel2.txt"
  scp -q xorinox_efs:$RAWC/$c/summary.txt "$OUT/$c-xorinox.txt"
done
log "done raw=$RAWS (intel1, intel2) $RAWC (xorinox)"
