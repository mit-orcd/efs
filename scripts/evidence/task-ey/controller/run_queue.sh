#!/bin/bash
# run_queue.sh: run queue.txt lines ("NAME COMMAND..." ; COMMAND is run_cell.sh args or "sh:SCRIPT ARGS") one at a
# time, in order, re-reading the file so lines can be appended. Stops when a postflight is not clean
# or the file STOP exists. Done lines are recorded in queue.done.
set -u
L=$HOME/work/efs-taskEY-login; A=$HOME/efs-artifacts/taskEY-20261109
while [ -n "${WAIT_FOR:-}" ] && ! grep -q DONE $A/ops/$WAIT_FOR.times 2>/dev/null; do sleep 30; done
while true; do
  [ -e $L/STOP ] && { echo "STOP file $(date -u +%FT%TZ)" >> $L/queue.log; exit 0; }
  line=$(grep -v "^#" $L/queue.txt | grep -vxF -f <(cat $L/queue.done 2>/dev/null; echo __none__) | head -1)
  [ -z "$line" ] && { sleep 60; continue; }
  name=${line%% *}; rest=${line#* }
  bad=""
  for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
    ssh -O check -o ControlPath=/tmp/efs-taskEY-mux-$(id -un)/$h $h 2>/dev/null || $L/task-ey/task_ey_ssh.sh $h true >/dev/null 2>&1 || bad="$bad $h"
  done
  if [ -n "$bad" ]; then echo "NO-MASTER on $(hostname) for:$bad before $name $(date -u +%FT%TZ); queue stopped" >> $L/queue.log; touch $L/STOP; exit 1; fi
  echo "masters ok on $(hostname) before $name" >> $L/queue.log
  echo "begin $name $(date -u +%FT%TZ)" >> $L/queue.log
  case $rest in
    sh:*) eval "${rest#sh:}" ;;
    *) eval "$L/run_cell.sh $name $rest" ;;
  esac
  echo "$line" >> $L/queue.done
  echo "end $name $(date -u +%FT%TZ)" >> $L/queue.log
  post=$A/ops/$name.postflight.txt
  if [ -e $post ] && grep -qvE "listen18-19xxx=0 cluster_tree=absent live_entries=0 efs_procs=0|^postflight" $post; then
    echo "DIRTY postflight after $name; queue stopped" >> $L/queue.log; exit 1
  fi
done
