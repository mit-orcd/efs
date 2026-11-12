#!/bin/bash
# run_cell.sh NAME CLUSTER.json [qualify run args...]: one Task GD cell, run from the Mac with pre/post-flight of the
# five hosts. Output in $GD_OUT/NAME, then streamed to ~erbmi1/efs-artifacts/taskGD-20261111/NAME/ with a SHA256SUMS file.
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); H=$R/scripts/evidence/task-gd; OUT=${GD_OUT:-/tmp/gd/out}
ART='$HOME/efs-artifacts/taskGD-20261111'
name=$1; cluster=$2; shift 2; mkdir -p $OUT/ops
flight() { for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
  $H/task_gd_ssh.sh $h 'echo $(hostname) listen18-19xxx=$(ss -ltnH | grep -cE ":1[89][0-9]{3} ") trees=$(ls -d /tmp/efs-taskGD /tmp/efs-taskGD-tmp 2>/dev/null | wc -l) efs_procs=$(ps -u erbmi1 -o comm= | grep -cxE "efsd|efs-fuse") load=$(cut -d" " -f1 /proc/loadavg)' 2>&1 | grep -v cargo/env; done; }
{ echo "preflight $(date -u +%FT%TZ)"; flight; } > $OUT/ops/$name.preflight.txt
echo "start $(date -u +%FT%TZ)" > $OUT/ops/$name.times
cd $R && export GD_REPO=$R && python3 scripts/qualify.py run --cluster $cluster --out $OUT/$name "$@" > $OUT/ops/$name.driver.log 2>&1
rc=$?
echo "end $(date -u +%FT%TZ) rc=$rc" >> $OUT/ops/$name.times
{ echo "postflight $(date -u +%FT%TZ)"; flight; } > $OUT/ops/$name.postflight.txt
$H/push.sh $name > $OUT/ops/$name.push.txt 2>&1
echo "DONE $name rc=$rc" >> $OUT/ops/$name.times
