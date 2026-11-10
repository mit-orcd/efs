#!/bin/bash
# fi_quiet.sh [DEADLINE_S] — Task FI quiet-host gate: three consecutive checks
# 20 s apart in which no other task's efsd, efs-fuse, ior, mpirun, perf,
# make_test_summary, test or efs-bench process runs. Own trees (/tmp/efs-taskFI)
# and the live orcd-mini mount are excluded by path. Prints the foreign
# processes while waiting and "quiet" with the load average when it passes;
# exit 1 when DEADLINE_S (default 7200) passes first. Waiting is the rule's
# own 20 s spacing, not a retry-to-green.
end=$(( $(date +%s) + ${1:-7200} )); ok=0
foreign() {
  ps -eo pid=,user=,args= | grep -E '(^| |/)(efsd|efs-fuse|ior|mpirun|prterun|perf|efs-bench)( |$)|make_test_summary|tests/test_|/test_[a-z0-9_]+( |$)' \
    | grep -v -e '/tmp/efs-taskFI' -e '/data1/efs/orcd-mini-mnt' -e 'grep -E' -e 'fi_quiet.sh' -e "^ *$$ "
}
while [ "$(date +%s)" -lt "$end" ]; do
  f=$(foreign)
  if [ -z "$f" ]; then ok=$((ok+1)); echo "check $ok/3 clean $(cut -d' ' -f1-3 /proc/loadavg)"
  else ok=0; echo "busy: $(echo "$f" | wc -l) foreign processes, e.g. $(echo "$f" | head -1 | cut -c1-150)"; fi
  [ $ok -ge 3 ] && { echo "quiet load=$(cut -d' ' -f1-3 /proc/loadavg)"; exit 0; }
  sleep 20
done
echo "not quiet within the deadline"; exit 1
