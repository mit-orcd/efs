#!/bin/bash
# run_dw.sh NAME ARM RUNS : H7 shared-chunk campaign (Task DW controller) on fcstor012-014 + fcstor010/011.
set -u
L=$HOME/work/efs-taskEY-login; A=$HOME/efs-artifacts/taskEY-20261109; name=$1; arm=$2; runs=$3
source <(sed -n "/^flight()/,/^}/p" $L/run_h.sh)
{ echo "preflight $(date -u +%FT%TZ)"; flight; } > $A/ops/$name.preflight.txt
echo "start $(date -u +%FT%TZ)" > $A/ops/$name.times
mkdir -p $A/$name
DW_SSH=$L/task-ey/dw_ssh.sh DW_BIN=$L/dwbin DW_ART=$A/$name DW_RUNROOT=/tmp/efs-taskEY/dw DW_STOREROOT=/tmp/efs-taskEY/dw-stores DW_TMP=/tmp/efs-taskEY/dw-tmp \
  python3.12 $L/task-ey/k38_dw_campaign.py --arm $arm --runs $runs --out $A/$name/out > $A/ops/$name.driver.log 2>&1
rc=$?
echo "end $(date -u +%FT%TZ) rc=$rc" >> $A/ops/$name.times
for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do $HOME/work/efs-taskEY-login/task-ey/task_ey_ssh.sh $h "for d in /tmp/efs-taskEY/dw /tmp/efs-taskEY/dw-stores /tmp/efs-taskEY/dw-tmp; do test -e \$d && ~/bin/edelete --force --delete \$d >/dev/null; done; true" 2>/dev/null; done
{ echo "postflight $(date -u +%FT%TZ)"; flight; } > $A/ops/$name.postflight.txt
echo "DONE $name rc=$rc" >> $A/ops/$name.times
