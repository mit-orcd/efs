#!/bin/bash
# d4_cpu_sidecar.sh RUN HOSTS... : while RUNs d4-progress gate runs, every 15 s record each member hosts efsd
# CPU time (process and its five busiest threads) to ARTIFACTS/RUN-d4-cpu/samples.txt. Observation only.
set -u
A=$HOME/efs-artifacts/taskEY-20261109; run=$1; shift
log=$A/ops/$run.driver.log; out=$A/$run-d4-cpu; 
until grep -q "gate d4-progress start" $log 2>/dev/null; do sleep 5; done
mkdir -p $out; echo "start $(date -u +%FT%TZ) hosts=$*" > $out/samples.txt
while ! grep -qE "gate d4-progress (PASS|FAIL|NOTRUN)" $log; do
  t=$(date -u +%FT%TZ)
  for h in "$@"; do
    $HOME/work/efs-taskEY-login/task-ey/task_ey_ssh.sh $h "p=\$(pgrep -u erbmi1 -x efsd | head -1); [ -n \"\$p\" ] || exit 0; echo \"$t \$(hostname) pid=\$p proc_time=\$(ps -o time= -p \$p) args=\$(ps -o args= -p \$p | grep -o -- \"--listen [^ ]*\\|^[^ ]*efsd [0-9.:]*\")\"; ps -L -o tid=,time= -p \$p | sort -k2 | tail -5 | sed \"s/^/$t \$(hostname) thread /\"" 2>&1 | grep -v cargo/env >> $out/samples.txt
  done
  sleep 15
done
echo "end $(date -u +%FT%TZ)" >> $out/samples.txt
