#!/bin/bash
# run_h.sh NAME HOST TRANSPORT CMDFILE : an own run. CMDFILE lines "label<TAB>repeats<TAB>command" run in the
# candidate tree on HOST, one at a time, with TMPDIR=/tmp/efs-taskGD/h/NAME/label-i, EFS_TRANSPORT=TRANSPORT
# (and EFS_RDMA_DEV=mlx5_2 for rdma). Writes ARTIFACTS/NAME/{label-i.log,results.tsv}; a failed command keeps the
# text logs of its TMPDIR under label-i.tmplogs/. Every TMPDIR is removed afterwards.
set -u
H=$(cd "$(dirname "$0")" && pwd); OUT=${GD_OUT:-/tmp/gd/out}; A=/home/erbmi1/efs-artifacts/taskGD-20261111
name=$1; host=$2; tr=$3; cmds=$4
out=$OUT/$name; mkdir -p $out $OUT/ops; cp $cmds $out/commands.tsv
flight() {
  for h in fcstor010 fcstor011 fcstor012 fcstor013 fcstor014; do
    for try in 1 2 3; do r=$($H/task_gd_ssh.sh $h "echo \$(hostname) listen18-19xxx=\$(ss -ltnH | grep -cE \":1[89][0-9]{3} \") cluster_tree=\$(test -e /tmp/efs-taskGD/cluster && echo present || echo absent) live_entries=\$(ls -A /tmp/efs-taskGD/live 2>/dev/null | wc -l) efs_procs=\$(ps -u erbmi1 -o comm= | grep -cxE \"efsd|efs-fuse\") load=\$(cut -d\" \" -f1 /proc/loadavg)"  2>&1 | grep -v cargo/env) && { echo "$r"; break; }; echo "$h ssh attempt $try failed: $r"; sleep 10; done
  done
}
{ echo "preflight $(date -u +%FT%TZ)"; flight; } > $OUT/ops/$name.preflight.txt
echo "start $(date -u +%FT%TZ) host=$host transport=$tr" > $OUT/ops/$name.times
printf "label\ti\trc\tseconds\tstart\trdma_up_logs\ttcp_logs\n" > $out/results.tsv
while IFS=$(printf "\t") read -r label reps cmd; do
  [ -z "$label" ] && continue; case $label in \#*) continue;; esac
  for i in $(seq 1 $reps); do
    t0=$(date -u +%FT%TZ); s0=$(date +%s)
    $H/task_gd_ssh.sh $host bash -s > $out/$label-$i.log 2>&1 <<EOR
S=\$HOME/work/efs-taskGD-fcstor012/src; F=\$HOME/work/efs-taskGD-fcstor012/faults
T=/tmp/efs-taskGD-tmp/$name/$label-$i; mkdir -p \$T; export TMPDIR=\$T EFS_TRANSPORT=$tr
[ $tr = rdma ] && export EFS_RDMA_DEV=mlx5_2
cd \$S; echo "== \$(hostname) \$(date -u +%FT%TZ) EFS_TRANSPORT=\$EFS_TRANSPORT EFS_RDMA_DEV=\${EFS_RDMA_DEV:-} TMPDIR=\$T"; echo "== $cmd"
( $cmd ); rc=\$?
up=\$(grep -rl --include="*.log" "transport rdma: rdma on" \$T 2>/dev/null | wc -l); tl=\$(grep -rlE --include="*.log" "transport tcp" \$T 2>/dev/null | wc -l)
if [ \$rc -ne 0 ]; then mkdir -p $A/$name/$label-$i.tmplogs; (cd \$T && find . -maxdepth 6 -type f \( -name "*.log" -o -name "*.json" -o -name "*.txt" -o -name "*.tsv" \) -size -50M -exec cp --parents {} $A/$name/$label-$i.tmplogs/ \;); fi
echo "== rc=\$rc rdma_up_logs=\$up tcp_logs=\$tl"
for m in \$(awk -v t="\$T" "index(\\\$2,t)==1{print \\\$2}" /proc/mounts); do fusermount3 -uz \$m; echo "== lazy-unmounted stuck \$m"; done
~/bin/edelete --force --delete \$T > /dev/null; rmdir /tmp/efs-taskGD-tmp/$name 2>/dev/null; true
EOR
    s1=$(date +%s); line=$(grep "^== rc=" $out/$label-$i.log | tail -1)
    rc=$(echo "$line" | sed -n "s/.*rc=\([0-9]*\).*/\1/p"); up=$(echo "$line" | sed -n "s/.*rdma_up_logs=\([0-9]*\).*/\1/p"); tl=$(echo "$line" | sed -n "s/.*tcp_logs=\([0-9]*\).*/\1/p")
    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$label" "$i" "${rc:-ssh-fail}" "$((s1-s0))" "$t0" "${up:-}" "${tl:-}" >> $out/results.tsv
  done
done < $cmds
echo "end $(date -u +%FT%TZ)" >> $OUT/ops/$name.times
{ echo "postflight $(date -u +%FT%TZ)"; flight; } > $OUT/ops/$name.postflight.txt
$H/push.sh $name > $OUT/ops/$name.push.txt 2>&1
echo "DONE $name" >> $OUT/ops/$name.times
