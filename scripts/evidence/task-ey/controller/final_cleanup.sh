#!/bin/bash
# final_cleanup.sh: end of Task EY. Remove /tmp/efs-taskEY on fcstor010-014 (refuses while a mount or a campaign
# process remains), remove the candidate tree, close the per-host master connections, record what is left, stop the queue.
set -u
L=$HOME/work/efs-taskEY-login; A=$HOME/efs-artifacts/taskEY-20261109; O=$A/ops/final-cleanup.txt
{
echo "final cleanup on $(hostname) $(date -u +%FT%TZ)"
for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
  $L/task-ey/task_ey_ssh.sh $h 'h=$(hostname -s); [ -e /tmp/efs-taskEY ] || { echo "$h: /tmp/efs-taskEY absent"; exit 0; }
    if grep -q " /tmp/efs-taskEY" /proc/mounts; then echo "$h: mount under /tmp/efs-taskEY, tree KEPT"; exit 0; fi
    if pgrep -u erbmi1 -x efsd >/dev/null || pgrep -u erbmi1 -x efs-fuse >/dev/null; then echo "$h: campaign process left, tree KEPT"; exit 0; fi
    ~/bin/edelete --force --delete /tmp/efs-taskEY >/dev/null; echo "$h: /tmp/efs-taskEY rc=$? left=$(ls -d /tmp/efs-taskEY 2>/dev/null || echo none)"' 2>&1 | grep -v cargo/env
done
[ -d $HOME/work/efs-taskEY-fcstor012 ] && { $HOME/bin/edelete --force --delete $HOME/work/efs-taskEY-fcstor012 >/dev/null; echo "candidate tree rc=$? left=$(ls -d $HOME/work/efs-taskEY-fcstor012 2>/dev/null || echo none)"; }
[ -e /tmp/efs-taskEY ] && $HOME/bin/edelete --force --delete /tmp/efs-taskEY >/dev/null
for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
  ssh -O exit -o ControlPath=/tmp/efs-taskEY-mux-$(id -un)/$h $h 2>&1 | sed "s/^/$h: /"
done
rmdir /tmp/efs-taskEY-mux-$(id -un) 2>/dev/null
echo "control sockets left: $(ls -A /tmp/efs-taskEY-mux-$(id -un) 2>/dev/null | wc -l) (dir $( [ -d /tmp/efs-taskEY-mux-$(id -un) ] && echo present || echo absent))"
echo "ssh processes of $(id -un) left on $(hostname):"; pgrep -u $(id -un) -x ssh -a || echo none
echo "done $(date -u +%FT%TZ)"
} > $O 2>&1
touch $L/STOP
