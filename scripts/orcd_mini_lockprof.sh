#!/bin/bash
# orcd_mini_lockprof.sh DIR — run on node9901; profiles the deployed orcd-mini efsd under whatever
# workload is running: Raft commit rate (10 s), DWARF context-switch capture
# of each group leader (3 s), frame-pointer cs on all efsd (10 s), on-CPU
# of the group-0 leader (10 s, 499 Hz). Raw data stays in DIR.
set -u; H=$(cd "$(dirname "$0")" && pwd); D=$1; mkdir -p "$D"; cd "$D"
M=/data1/efs/src/efs-mgmt
declare -A port=([1]=17432 [2]=17433 [3]=17434)
pid_of() { pgrep -f "^/data1/efs/src/efsd .*--port ${port[$1]}( |$)" | head -1; }
$M raft-status 10.1.99.1:17432 > rs0.txt; date +%s.%N >> rs0.txt; sleep 10
$M raft-status 10.1.99.1:17432 > rs1.txt; date +%s.%N >> rs1.txt
python3 - <<"PY"
import re
def rd(f):
    l=open(f).read().split("\n"); t=float([x for x in l if x.strip()][-1])
    g={int(m[0]):(int(m[1]),int(m[2])) for m in re.findall(r"group (\d+) .*leader=(\d+) .*commit=(\d+)",open(f).read())}
    return t,g
t0,a=rd("rs0.txt"); t1,b=rd("rs1.txt")
for k in sorted(a): print("group %d commits/s=%.0f"%(k,(b[k][1]-a[k][1])/(t1-t0)))
PY
lead_of() { for i in 1 2 3; do $M raft-status 10.1.99.1:${port[$i]} 2>/dev/null | grep -q "^  group $1 .*role=LEADER" && echo $i; done | head -1; }
l0=$(lead_of 0); l2=$(lead_of 2); leaders=$(printf "%s\n%s\n" $l0 $l2 | sort -u)
echo "role=LEADER: group 0 n$l0, group 2 n$l2"
for l in $leaders; do
  perf record -e cs --call-graph dwarf,16384 -p "$(pid_of $l)" -o dwarf-n$l.data -- sleep 3 > /dev/null 2>&1
done
all=$(for i in 1 2 3; do pid_of $i; done | paste -sd,)
perf record -e cs -g -p "$all" -o cs-all.data -- sleep 10 > /dev/null 2>&1
perf record -F 499 -g -p "$(pid_of $l0)" -o oncpu-n$l0.data -- sleep 10 > /dev/null 2>&1
for l in $leaders; do echo "== DWARF cs, leader n$l (3 s)"; perf script -i dwarf-n$l.data 2>/dev/null | python3 "$H/cs_dwarf_locks.py"; done
echo "== fp cs, all efsd (10 s)"; perf script -i cs-all.data 2>/dev/null | python3 "$H/cs_lock.py"
echo "== on-CPU leader n$l0 (10 s), top self symbols"
perf report -i oncpu-n$l0.data --no-children --stdio --sort sym 2>/dev/null | grep -E "^ +[0-9]" | head -25
