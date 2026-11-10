#!/bin/bash
# orcd_mini_o1_windows.sh MODE REPS OUTDIR — batch O1/O2 on orcd-mini, one
# rep at a time, with each client's log window per rep (Task EW).
#   MODE hard|easy: orcd_mini_batch_o_ior.sh MODE 1 on node9900 (detached
#        screen), OUTDIR/MODE-<i>/ holds its output (ior's in MODE-1.txt);
#   MODE shared: orcd_mini_shared_chunk.py write on both nodes at once
#        (node9901 ranks 0-3, node9900 ranks 4-7, 8 writers, 2000 records),
#        then verify through both mounts.
# OUTDIR is a path under the NFS home both nodes share (e.g.
# $HOME/efs-artifacts/<task>/o1); per rep it gets client-node990{0,1}.log
# (the bytes each efs-fuse log gained during the rep) and efsd-nN.log, and
# windows.txt gets one line per rep with the counts the O1 acceptance reads.
# Driver on the Mac; scripts come from each node's deployed release (src).
set -u
MODE=$1; REPS=$2; O=$3
S=$(cd "$(dirname "$0")" && pwd)/orcd_mini_ssh.sh
q() { "$S" "$@" 2>&1 | grep -v -e NOTICE -e 'scheduled maintenance' -e '^\*\*\*' -e '^ *$'; }
L00=/data1/efs/orcd-mini-client/logs/efs-fuse-orcd-mini-mnt.log
L01=/data1/efs/efs-fuse-mnt.log
SRC00=/data1/efs/orcd-mini-client/src; SRC01=/data1/efs/src
size() { q "$1" "stat -c %s $2"; }
pat='EIO|report-split stale|rc=-14|publish-stalled|flush-meta'
fail=0
for i in $(seq 1 "$REPS"); do
  R=$O/$MODE-$i
  o00=$(size node9900 $L00); o01=$(size node9901 $L01)
  oe=$(q node9901 'for n in 1 2 3 4; do stat -c %s /data1/efs/logs/efsd-n$n.log; done' | tr '\n' ' ')
  t0=$(date +%s)
  case $MODE in
  hard|easy)
    q node9900 "mkdir -p $O; mkdir -p -m 700 \$HOME/.screen; cd /tmp && SCREENDIR=\$HOME/.screen setsid screen -dmS ew-o1-$MODE-$i bash -c '$SRC00/scripts/evidence/orcd-mini/orcd_mini_batch_o_ior.sh $MODE 1 $R ew$i > $R.out 2>&1; echo RC=\$? >> $R.out' < /dev/null > /dev/null 2>&1"
    until q node9900 "grep -q ^RC= $R.out 2>/dev/null && echo y" | grep -q y; do sleep 15; done
    rc=$(q node9900 "sed -n 's/^RC=//p' $R.out")
    ;;
  shared)
    d=ew-sc-$i-$t0
    q node9901 "mkdir -p $R /data1/efs/mnt/$d"
    for h in node9901 node9900; do
      [ $h = node9901 ] && { m=/data1/efs/mnt; rk=0-3; src=$SRC01; } || { m=/data1/efs/orcd-mini-mnt; rk=4-7; src=$SRC00; }
      q $h "mkdir -p -m 700 \$HOME/.screen; cd /tmp && SCREENDIR=\$HOME/.screen setsid screen -dmS ew-sc-$i-$h bash -c 'until [ -d $m/$d ]; do sleep 1; done; python3 $src/scripts/evidence/orcd-mini/orcd_mini_shared_chunk.py write $m/$d/file --writers 8 --ranks $rk > $R/write-$h.txt 2>&1; echo RC=\$? >> $R/write-$h.txt' < /dev/null > /dev/null 2>&1"
    done
    until q node9901 "grep -q ^RC= $R/write-node9901.txt $R/write-node9900.txt 2>/dev/null && [ \$(cat $R/write-*.txt | grep -c ^RC=) = 2 ] && echo y" | grep -q y; do sleep 10; done
    q node9901 "python3 $SRC01/scripts/evidence/orcd-mini/orcd_mini_shared_chunk.py verify /data1/efs/mnt/$d/file --writers 8 --ranks 0-7 > $R/verify-node9901.txt 2>&1; echo RC=\$? >> $R/verify-node9901.txt"
    q node9900 "python3 $SRC00/scripts/evidence/orcd-mini/orcd_mini_shared_chunk.py verify /data1/efs/orcd-mini-mnt/$d/file --writers 8 --ranks 0-7 > $R/verify-node9900.txt 2>&1; echo RC=\$? >> $R/verify-node9900.txt"
    rc=$(q node9901 "cat $R/write-*.txt $R/verify-*.txt | grep ^RC= | grep -vc '^RC=0$'")
    # a failed run keeps its file: it is the only record of which bytes were wrong
    [ "$rc" = 0 ] && q node9901 "rm -f /data1/efs/mnt/$d/file && rmdir /data1/efs/mnt/$d"
    ;;
  esac
  sec=$(( $(date +%s) - t0 ))
  q node9900 "mkdir -p $R; tail -c +$((o00+1)) $L00 > $R/client-node9900.log"
  ce=""; n=1; for off in $oe; do ce="$ce tail -c +$((off+1)) /data1/efs/logs/efsd-n$n.log > $R/efsd-n$n.log;"; n=$((n+1)); done
  q node9901 "tail -c +$((o01+1)) $L01 > $R/client-node9901.log;$ce"
  c=$(q node9901 "for f in $R/client-node9900.log $R/client-node9901.log; do printf '%s=%s ' \$(basename \$f .log) \$(grep -cE '$pat' \$f); done; printf 'efsd=%s' \$(cat $R/efsd-n*.log | grep -cE '$pat')")
  echo "$MODE rep=$i rc=$rc seconds=$sec $c"
  q node9901 "echo '$MODE rep=$i rc=$rc seconds=$sec $c' >> $O/windows.txt"
  [ "$rc" = 0 ] || fail=$((fail+1))
done
exit $fail
