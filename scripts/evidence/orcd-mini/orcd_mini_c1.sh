#!/bin/bash
# orcd_mini_c1.sh SECONDS WAIT_A WAIT_B
#
# W125 closure C1 on orcd-mini: modify packed files under load, two
# clients, armed node-side (named screens; nothing depends on the Mac after
# arming). Each node waits until its WAIT file contains a line starting
# with RC= (the previous run finished), then:
#   node9901 (client A, /data1/efs/mnt/c1-<tag>/c): w125_concurrent.py with
#     4 writers, 4 readers, 2 deleters and 4 modifiers for SECONDS; a
#     compaction loop (efs-mgmt pack-compact every 60 s); when A ends and B's
#     deletion list has arrived through efs, the cross-check (A's missing
#     files must be B's deletions, none may reappear) and cleanup;
#   node9900 (client B, /data1/efs/orcd-mini-mnt/c1-<tag>/c): --remote with
#     4 version-tolerant readers and 2 deleters; when it ends, its deletion
#     list is handed over through /data1/efs/orcd-mini-mnt/c1-<tag>/b.gone.
# Output: node9901 /data1/efs/prof/c1-<tag>, node9900
# /data1/efs/orcd-mini-client/prof/c1-<tag> (result.txt on node9901).
set -u
SEC=$1; WA=$2; WB=$3; TAG=$(date -u +%Y%m%dT%H%M)
S=$(cd "$(dirname "$0")" && pwd)/orcd_mini_ssh.sh; HERE=$(cd "$(dirname "$0")" && pwd)
q() { "$S" "$@" 2>&1 | grep -v -e NOTICE -e 'scheduled maintenance' -e '^\*\*\*' -e '^ *$'; }
DA=/data1/efs/prof/c1-$TAG; DB=/data1/efs/orcd-mini-client/prof/c1-$TAG
MA=/data1/efs/mnt/c1-$TAG; MB=/data1/efs/orcd-mini-mnt/c1-$TAG
q node9901 "mkdir -p $DA && cat > $DA/w125_concurrent.py" < "$HERE/w125_concurrent.py"
q node9900 "mkdir -p $DB && cat > $DB/w125_concurrent.py" < "$HERE/w125_concurrent.py"
q node9901 "cat > $DA/run.sh && chmod +x $DA/run.sh" <<EOF
#!/bin/bash
cd $DA; until grep -q ^RC= $WA 2>/dev/null; do sleep 15; done
mkdir -p $MA; date -u +%T > started.txt
screen -dmS c1-$TAG-comp bash -c 'while [ ! -f $DA/a.done ]; do echo "\$(date -u +%T) \$(/data1/efs/src/efs-mgmt pack-compact 10.1.99.1:17432 64 2>&1 | grep -o "node [0-9].*leads group.*" | tr "\n" " ")" >> $DA/compact.log; sleep 60; done'
python3 w125_concurrent.py $MA/c $SEC --writers 4 --readers 4 --deleters 2 --modifiers 4 > a.txt 2>&1; echo RC=\$? >> a.txt; touch a.done
until [ -f $MA/b.gone ]; do sleep 10; done
python3 - $MA/c $MA/b.gone > crosscheck.txt 2>&1 <<'PY'
import os, sys
root, gonef = sys.argv[1], sys.argv[2]
miss = set(filter(None, open(root + '.missing').read().split('\n')))
gone = set(filter(None, open(gonef).read().split('\n')))
present = [g for g in gone if os.path.exists(os.path.join(root, g))]
print('A_missing=%d B_deleted=%d missing_not_deleted_by_B=%d B_deleted_still_present=%d' % (len(miss), len(gone), len(miss - gone), len(present)))
sys.exit(1 if miss - gone or present else 0)
PY
echo "crosscheck rc=\$?" >> crosscheck.txt
{ grep -E "^final|^ERR|^RC=" a.txt; cat crosscheck.txt; } > result.txt
rm -rf $MA
echo "finished \$(date -u +%T)" >> result.txt
EOF
q node9900 "cat > $DB/run.sh && chmod +x $DB/run.sh" <<EOF
#!/bin/bash
cd $DB; until grep -q ^RC= $WB 2>/dev/null; do sleep 15; done
sleep 60; date -u +%T > started.txt
python3 w125_concurrent.py $MB/c $((SEC - 60)) --remote --readers 4 --deleters 2 --deleted-out $DB/b.gone > b.txt 2>&1; echo RC=\$? >> b.txt
cp b.gone $MB/b.gone.tmp && mv $MB/b.gone.tmp $MB/b.gone; echo handed-over >> b.txt
EOF
q node9901 "screen -dmS c1-$TAG $DA/run.sh && screen -ls | grep c1-$TAG"
q node9900 "screen -dmS c1-$TAG $DB/run.sh && screen -ls | grep c1-$TAG"
echo "c1-$TAG armed (${SEC} s)"
