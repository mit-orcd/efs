#!/bin/bash
# orcd_mini_workload_profile.sh LABEL [SECONDS]
#
# Run from the Mac. Starts the user's orcd-mini tiny-file workload on the
# node9900 client, then profiles the deployed orcd-mini daemons under it:
#   - writer: tiny_files.py, 16 workers, into a new tiny-files-prof-LABEL dir;
#   - reader: tiny_files_md5.py, 16 workers, over tiny-files1;
#   - crawler: ecrawl --no-write over the mount.
# After a 60 s warm-up it records context-switch samples of all three efsd
# (10 s, mutex waits by caller), the 200-file latency probe on both mounts
# at the same time, and the efsd CPU over 15 s; then it stops the workload
# (SECONDS caps it, default 180) and prints the writer's and reader's rates.
# Raw output stays outside Git: node9901 /data1/efs/prof/wl-LABEL and node9900
# /data1/efs/orcd-mini-client/prof/wl-LABEL. Prerequisites: node9901
# /data1/efs/prof/probe.py and cs_lock.py (from orcd_mini_readindex_ab.sh), the
# same probe.py on node9900, and ssh wrappers in N99 and N00. Afterwards remove
# the writer's tiny-files-prof-LABEL directory if it is not wanted.
set -u
L=$1; T=${2:-180}
N99=${N99:?ssh wrapper for node9901}; N00=${N00:?ssh wrapper for node9900}
M=/data1/efs/orcd-mini-mnt; CD=/data1/efs/orcd-mini-client/prof/wl-$L; SD=/data1/efs/prof/wl-$L
$N00 "mkdir -p $CD && cat > $CD/launch.sh" <<EOF
cd $CD
timeout $T python3 /data1/efs/testing/tiny_files.py --max-depth 8 --max-files-per-folder 1000 \
  --max-files 2000000 --workers 16 $M/tiny-files-prof-$L > writer.log 2>&1 < /dev/null &
echo \$! > pids
timeout $T python3 /data1/efs/testing/tiny_files_md5.py $M/tiny-files1 --workers 16 \
  --output $CD/md5.jsonl > md5.log 2>&1 < /dev/null &
echo \$! >> pids
timeout $T \$HOME/git/ereport/ecrawl --no-write --progress $M/ > crawl.log 2>&1 < /dev/null &
echo \$! >> pids
date -u +%FT%TZ > started
wait
date -u +%FT%TZ > finished
EOF
$N00 "setsid nohup bash $CD/launch.sh > /dev/null 2>&1 < /dev/null & sleep 2; cat $CD/started; wc -l < $CD/pids"
sleep 60
$N99 "mkdir -p $SD; cd $SD; p=\$(pgrep -d, -f '^/data1/efs/src/efsd'); \
  perf record -e cs -g -p \$p -o cs.data -- sleep 10 > /dev/null 2>&1; \
  c0=0; for t in \${p//,/ }; do c0=\$((c0+\$(awk '{print \$14+\$15}' /proc/\$t/stat))); done; sleep 15; \
  c1=0; for t in \${p//,/ }; do c1=\$((c1+\$(awk '{print \$14+\$15}' /proc/\$t/stat))); done; \
  echo \"efsd CPU \$(( (c1-c0)/15 ))% of one core (15 s)\"; \
  perf script -i cs.data 2>/dev/null | python3 /data1/efs/prof/cs_lock.py" > ${TMPDIR:-/tmp}/wl-$L-cs.txt &
$N00 "cd $CD; python3 /data1/efs/orcd-mini-client/prof/probe.py $M/probe-$L-n00 200 > probe.txt 2>&1; echo '-- node9900 probe'; cat probe.txt" &
$N99 "mkdir -p $SD; cd $SD; python3 /data1/efs/prof/probe.py /data1/efs/mnt/probe-$L-n01 200 > probe.txt 2>&1; echo '-- node9901 probe'; cat probe.txt" &
wait
cat ${TMPDIR:-/tmp}/wl-$L-cs.txt
$N00 "cd $CD; kill \$(cat pids) 2>/dev/null; for i in \$(seq 30); do test -e finished && break; sleep 1; done; \
  echo '-- workload'; cat started finished; tail -c 200 writer.log | tr '\r' '\n' | tail -2; tail -c 300 md5.log | tr '\r' '\n' | tail -2"
