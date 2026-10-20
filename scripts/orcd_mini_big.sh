#!/bin/bash
# orcd_mini_big.sh start [HOURS] | arm HOURS WAIT_A WAIT_B | status TAG | collect TAG
#
# W125 big mixed test on orcd-mini. Every component runs in its own named
# screen session on the nodes (recoverable: the Mac only starts and reads):
#   node9901 (client A, /data1/efs/mnt):
#     big-<tag>-conc  w125_concurrent.py 8 writers / 8 verifying readers / 4 deleters
#     big-<tag>-dd    w125_big_node.sh dd       (dd 256 MiB-4 GiB, md5 checked)
#     big-<tag>-comp  w125_big_node.sh compact  (pack-compact every 60 s, status)
#     big-<tag>-efs   w125_big_node.sh efs      (.efs stats/find every 120 s)
#   node9900 (client B, /data1/efs/orcd-mini-mnt):
#     big-<tag>-conc  w125_concurrent.py --remote 8 readers / 4 deleters on A's files
#     big-<tag>-tiny  tiny_files.py, 100,000 files 1 B - 4 MiB, 16 workers
#     big-<tag>-md5   tiny_files_md5.py passes, each compared with the previous
#     big-<tag>-del   random_delete.py 30% of the tiny files every 10 min
#     big-<tag>-dd    w125_big_node.sh dd
# arm: the same, but each node gets a launcher screen (big-<tag>-launch)
# that waits until its WAIT file (node9901: WAIT_A, node9900: WAIT_B)
# contains a line starting with RC= (a previous run finished), then starts
# that node's components; nothing depends on the Mac after arming.
# Output: node9901 /data1/efs/prof/big-<tag>, node9900
# /data1/efs/orcd-mini-client/prof/big-<tag>. `collect` prints every log's
# summary, FAIL lines, the concurrent results and the A/B cross-check.
set -u
S=$(cd "$(dirname "$0")" && pwd)/orcd_mini_ssh.sh; HERE=$(cd "$(dirname "$0")" && pwd); T=~/git/cluster/testing
A=/data1/efs/prof; B=/data1/efs/orcd-mini-client/prof
q() { "$S" "$@" 2>&1 | grep -v -e NOTICE -e 'scheduled maintenance' -e '^\*\*\*' -e '^ *$'; }
case $1 in
start|arm)
  if [ "$1" = arm ]; then HOURS=$2; WA=$3; WB=$4; else HOURS=${2:-2}; WA=; WB=; fi
  TAG=$(date -u +%Y%m%dT%H%M); SEC=$(( HOURS * 3600 ))
  for f in w125_concurrent.py w125_md5_compare.py w125_big_node.sh; do
    q node9901 "mkdir -p $A/big-$TAG && cat > $A/big-$TAG/$f" < "$HERE/$f"
    q node9900 "mkdir -p $B/big-$TAG && cat > $B/big-$TAG/$f" < "$HERE/$f"
  done
  for f in tiny_files.py tiny_files_md5.py random_delete.py; do q node9900 "cat > $B/big-$TAG/$f" < "$T/$f"; done
  MA=/data1/efs/mnt/big-$TAG; MB=/data1/efs/orcd-mini-mnt/big-$TAG; DA=$A/big-$TAG; DB=$B/big-$TAG
  # Node launch scripts: the deadline is taken on the node when it launches.
  q node9901 "cat > $DA/launch.sh" <<LAUNCH
#!/bin/bash
cd $DA; END=\$(( \$(date +%s) + $SEC )); mkdir -p $MA
screen -dmS big-$TAG-conc bash -c 'python3 w125_concurrent.py $MA/c $SEC --writers 8 --readers 8 --deleters 4 > conc.txt 2>&1; echo RC=\$? >> conc.txt'
screen -dmS big-$TAG-dd bash -c "./w125_big_node.sh dd $MA $DA \$END"
screen -dmS big-$TAG-comp bash -c "./w125_big_node.sh compact $MA $DA \$END 10.1.99.1:17432 /data1/efs/src/efs-mgmt"
screen -dmS big-$TAG-efs bash -c "./w125_big_node.sh efs /data1/efs/mnt $DA \$END"
echo "launched \$(date -u +%T) until \$(date -u -d @\$END +%T)" > launched.txt
LAUNCH
  q node9900 "cat > $DB/launch.sh" <<LAUNCH
#!/bin/bash
cd $DB; sleep 30; END=\$(( \$(date +%s) + $SEC - 30 ))
screen -dmS big-$TAG-conc bash -c 'python3 w125_concurrent.py $MB/c $((SEC - 30)) --remote --readers 8 --deleters 4 --deleted-out $DB/b.gone > conc.txt 2>&1; echo RC=\$? >> conc.txt'
screen -dmS big-$TAG-tiny bash -c "./w125_big_node.sh tiny $MB $DB \$END $DB"
screen -dmS big-$TAG-md5 bash -c "./w125_big_node.sh md5 $MB $DB \$END $DB $DB/w125_md5_compare.py"
screen -dmS big-$TAG-del bash -c "./w125_big_node.sh del $MB $DB \$END $DB"
screen -dmS big-$TAG-dd bash -c "./w125_big_node.sh dd $MB $DB \$END"
echo "launched \$(date -u +%T) until \$(date -u -d @\$END +%T)" > launched.txt
LAUNCH
  for n in 9901 9900; do
    D=$DA; W=$WA; [ $n = 9900 ] && { D=$DB; W=$WB; }
    if [ -n "$W" ]; then
      q node$n "cd $D && chmod +x *.sh *.py && screen -dmS big-$TAG-launch bash -c 'until grep -q ^RC= $W 2>/dev/null; do sleep 15; done; ./launch.sh' && screen -ls | grep big-$TAG"
    else
      q node$n "cd $D && chmod +x *.sh *.py && ./launch.sh && screen -ls | grep -c big-$TAG"
    fi
  done
  echo "big-$TAG ${1}ed (${HOURS} h)"
  ;;
status|collect)
  TAG=$2; DA=$A/big-$TAG; DB=$B/big-$TAG
  echo "== node9901 (A)"; q node9901 "cd $DA; screen -ls | grep -c big-$TAG; tail -n 1 conc.txt; for f in dd efs; do echo \"$f: \$(grep -c . $f.log 2>/dev/null) ops, \$(grep -c FAIL $f.log 2>/dev/null) FAIL; last: \$(tail -n 1 $f.log 2>/dev/null)\"; done; tail -n 4 compact.log | cut -c1-230"
  echo "== node9900 (B)"; q node9900 "cd $DB; screen -ls | grep -c big-$TAG; tail -n 1 conc.txt; for f in dd md5 del tiny; do echo \"$f: \$(grep -c FAIL $f.log 2>/dev/null) FAIL; last: \$(tail -n 1 $f.log 2>/dev/null | cut -c1-200)\"; done; grep -h 'mismatches=[1-9]\|read_errors=[1-9]' md5.log | head -3"
  if [ "$1" = collect ]; then
    echo "== FAIL lines"; q node9901 "grep -h FAIL $DA/*.log | head"; q node9900 "grep -h FAIL $DB/*.log | head"
    echo "== concurrent"; q node9901 "grep -E 'final|ERR|RC=' $DA/conc.txt | head -12"; q node9900 "grep -E 'final|ERR|RC=' $DB/conc.txt | head -12"
    q node9900 "cat $DB/b.gone" | q node9901 "cat > $DA/b.gone"
    echo "== cross-check"; q node9901 "python3 - /data1/efs/mnt/big-$TAG/c $DA/b.gone" <<'EOF'
import os, sys
root, gonef = sys.argv[1], sys.argv[2]
miss = set(filter(None, open(root + ".missing").read().split("\n")))
gone = set(filter(None, open(gonef).read().split("\n")))
present = [g for g in gone if os.path.exists(os.path.join(root, g))]
print("A_missing=%d B_deleted=%d missing_not_deleted_by_B=%d B_deleted_still_present=%d" % (
    len(miss), len(gone), len(miss - gone), len(present)))
EOF
  fi
  ;;
*) echo "usage: $0 start [HOURS] | status TAG | collect TAG"; exit 2 ;;
esac
