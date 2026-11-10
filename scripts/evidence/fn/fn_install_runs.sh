#!/bin/bash
# Task FN follow-up: install time and longest pump step, before/after, three diff sizes.
# ~/efs-taskFN/src = devel cff8eb424 + fn_control_instrument.py (built: tests/faults/efsd efs-mgmt efs-bench);
# ~/efs-taskFN/t1 = the fix tree, built the same way. Ports 18860..18862. Output: ~/efs-artifacts/taskFN-20261110/i-*.
cd ~/efs-taskFN
A=~/efs-artifacts/taskFN-20261110
for sz in a:100000 b:200000 c:400000; do
  n=${sz%%:*}; files=${sz##*:}
  for t in before:src after:t1; do
    tag=${t%%:*}; tree=${t##*:}
    python3 ~/efs-taskFN/t1/scripts/evidence/fn/fn_import_rss.py --src ~/efs-taskFN/$tree --work ~/efs-taskFN/run-i-$tag-$n \
        --out $A/i-$tag-$n --files $files --port 18860 --memtable-bytes 2097152 > ~/efs-taskFN/i-$tag-$n.out 2>&1
    echo "$tag $n rc=$?" >> ~/efs-taskFN/runs.status
    ~/bin/edelete --force --delete ~/efs-taskFN/run-i-$tag-$n >/dev/null 2>&1
  done
done
echo done >> ~/efs-taskFN/runs.status
