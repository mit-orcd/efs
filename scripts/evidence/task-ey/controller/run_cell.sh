#!/bin/bash
# run_cell.sh NAME CLUSTER.json [qualify run args...] : one batch D cell, pre/post-flight, driver log.
set -u
L=$HOME/work/efs-taskEY-login; S=$HOME/work/efs-taskEY-fcstor012/src; A=$HOME/efs-artifacts/taskEY-20261109
name=$1; cluster=$2; shift 2
mkdir -p $A/ops
flight() {
  for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
    for try in 1 2 3; do r=$($HOME/work/efs-taskEY-login/task-ey/task_ey_ssh.sh $h "echo \$(hostname) listen18-19xxx=\$(ss -ltnH | grep -cE \":1[89][0-9]{3} \") cluster_tree=\$(test -e /tmp/efs-taskEY/cluster && echo present || echo absent) live_entries=\$(ls -A /tmp/efs-taskEY/live 2>/dev/null | wc -l) efs_procs=\$(ps -u erbmi1 -o comm= | grep -cxE \"efsd|efs-fuse\") load=\$(cut -d\" \" -f1 /proc/loadavg)" 2>&1 | grep -v cargo/env) && { echo "$r"; break; }; echo "$h ssh attempt $try failed: $r"; sleep 10; done
  done
}
{ echo "preflight $(date -u +%FT%TZ)"; flight; } > $A/ops/$name.preflight.txt 2>&1
echo "start $(date -u +%FT%TZ)" > $A/ops/$name.times
cd $S && python3.12 scripts/qualify.py run --cluster $cluster --out $A/$name "$@" > $A/ops/$name.driver.log 2>&1
rc=$?
echo "end $(date -u +%FT%TZ) rc=$rc" >> $A/ops/$name.times
{ echo "postflight $(date -u +%FT%TZ)"; flight; } > $A/ops/$name.postflight.txt 2>&1
echo "DONE $name rc=$rc" >> $A/ops/$name.times
