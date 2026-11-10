#!/bin/bash
# teardown.sh RUN : tear down a driver-owned cell the driver left up. Clean client stop first (force-discard and
# a lazy unmount only if refused), then each efsd of erbmi1 from the candidate tree by pid (SIGTERM, wait), logs
# copied to ARTIFACTS/RUN/manual-teardown, then the cluster and live trees removed. Never pkill.
set -u
A=$HOME/efs-artifacts/taskEY-20261109; S=$HOME/work/efs-taskEY-fcstor012/src; D=$A/$1/manual-teardown; mkdir -p $D
for h in fcstor010 fcstor011; do $HOME/work/efs-taskEY-login/task-ey/task_ey_ssh.sh $h "m=/tmp/efs-taskEY/cluster/mnt; mountpoint -q \$m || exit 0; cd $S
  scripts/client.sh stop \$m || { echo \"\$(hostname): clean stop refused; force-discard\"; scripts/client.sh stop --force-discard \$m; }
  mountpoint -q \$m && { fusermount3 -uz \$m; echo \"\$(hostname): lazy unmount\"; }; cp /tmp/efs-taskEY/cluster/efs-fuse-mnt.log $D/efs-fuse-\$(hostname).log 2>/dev/null; true" 2>&1 | grep -v cargo/env; done
for h in fcstor010 fcstor011 fcstor014 fcstor013 fcstor012; do $HOME/work/efs-taskEY-login/task-ey/task_ey_ssh.sh $h "for p in \$(pgrep -u erbmi1 -x efsd); do a=\$(tr \"\\0\" \" \" < /proc/\$p/cmdline); case \"\$a\" in *efs-taskEY*) ;; *) continue;; esac; echo \"\$(hostname) stop pid \$p: \$a\"; kill -TERM \$p; for i in \$(seq 1 90); do kill -0 \$p 2>/dev/null || break; sleep 1; done; kill -0 \$p 2>/dev/null && echo \"\$(hostname) pid \$p STILL ALIVE\"; done
  for f in /tmp/efs-taskEY/cluster/*.log; do [ -e \"\$f\" ] && cp \"\$f\" $D/\$(hostname)-\$(basename \$f); done
  pgrep -u erbmi1 -x efsd >/dev/null && echo \"\$(hostname): efsd left, trees kept\" && exit 0
  for t in /tmp/efs-taskEY/cluster /tmp/efs-taskEY/live/*; do [ -e \"\$t\" ] && ~/bin/edelete --force --delete \"\$t\" >/dev/null; done; true" 2>&1 | grep -v cargo/env; done
source <(sed -n "/^flight()/,/^}/p" $HOME/work/efs-taskEY-login/run_h.sh); flight | tee $D/after.txt
