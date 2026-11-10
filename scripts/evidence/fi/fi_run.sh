#!/bin/bash
# fi_run.sh LABEL TREE OUTDIR PORT [EASY_MB] — one Task FI measured run on a
# fresh private cluster: quiet gate, df/load, cluster start, 1 Hz sampler,
# ior easy exactly as Task EW ran it (8 ranks, -v -a POSIX -F -e -w -r -t 1m
# -b EASY_MB, default 2048), cluster stop, stores deleted. Output in
# OUTDIR/LABEL/: ior.txt, window.txt (write/read epoch), sample.tsv,
# efsd-n{1,2,3}.log, fuse.log, meta.txt. Exit status: ior's.
. /usr/mpi/gcc/openmpi-5.0.10rc2.2605121430/bin/mpivars.sh; set -u
L=$1; TREE=$2; O=$3/$1; P=$4; MB=${5:-2048}
HERE=$(cd "$(dirname "$0")" && pwd)
D=/tmp/efs-taskFI-run-$L; IOR=$HOME/orcd/scratch/efs-io500/io500/bin/ior
export TMPDIR=/tmp/efs-taskFI-tmp
mkdir -p "$O" && cd "$O" || exit 2
echo "label=$L tree=$TREE build=$(cat "$TREE/.build_id.stamp" 2>/dev/null)" > meta.txt
bash "$HERE/fi_quiet.sh" "${FI_QUIET_S:-7200}" > quiet.txt; [ $? = 0 ] || { echo "NOT QUIET" >> meta.txt; exit 3; }
{ echo "start=$(date -u +%FT%TZ) load_before=$(cut -d' ' -f1-3 /proc/loadavg)"; df -h /tmp | tail -1; } >> meta.txt
bash "$HERE/fi_cluster.sh" start "$TREE" "$D" "$P" >> meta.txt 2>&1 || { echo "CLUSTER START FAILED" >> meta.txt; bash "$HERE/fi_cluster.sh" stop "$TREE" "$D" "$P" >> meta.txt 2>&1; exit 4; }
args=$(awk '{printf "%s=%s ", $1, $2}' "$D/logs/pids")
python3 "$HERE/fi_sample.py" sample.tsv $args & sp=$!
mkdir -p "$D/mnt/ior"
rs() { for i in 1 2 3; do "$TREE/efs-mgmt" raft-status 127.0.0.1:$((P+i)) > "rs-$1-n$i.txt" 2>&1; done; }
# FI_PLACE=G0NODE:G2NODE moves the two metadata group leaders to those node ids
# (efs-mgmt raft-step-down), so a placement is a controlled variable; unset
# leaves the elected placement, which rs-before-n*.txt records either way.
if [ -n "${FI_PLACE:-}" ]; then
  for g in 0 2; do
    want=$(echo "$FI_PLACE" | cut -d: -f$([ $g = 0 ] && echo 1 || echo 2))
    for t in $(seq 60); do
      rs now; cur=""
      for i in 1 2 3; do grep -q "group $g hosted=[0-9]* role=LEADER" "rs-now-n$i.txt" && cur=$i; done
      [ -n "$cur" ] && [ "$(sed -n '1s/^node \([0-9]*\) .*/\1/p' "rs-now-n$cur.txt")" = "$want" ] && break
      [ -n "$cur" ] && [ $((t % 5)) = 1 ] && "$TREE/efs-mgmt" raft-step-down 127.0.0.1:$((P+cur)) $g $want >> place.txt 2>&1
      sleep 1
    done
  done
fi
rs before
mpirun -np 8 --host localhost:8 "$IOR" -v -a POSIX -F -e -w -r -t 1m -b ${MB}m -o "$D/mnt/ior/f" > ior.txt 2>&1; rc=$?
touch sample.tsv.stop; wait $sp
rs after
grep -aE "^Commencing (write|read)" ior.txt > window.txt
rm -rf "$D/mnt/ior"
for i in 1 2 3; do cp "$D/logs/efsd-n$i.log" efsd-n$i.log; done; cp "$D/logs/fuse.log" fuse.log
bash "$HERE/fi_cluster.sh" stop "$TREE" "$D" "$P" >> meta.txt 2>&1
rm -rf "$D"
echo "end=$(date -u +%FT%TZ) load_after=$(cut -d' ' -f1-3 /proc/loadavg) rc=$rc" >> meta.txt
grep -aE "^(write|read) " ior.txt >> meta.txt
exit $rc
