#!/bin/bash
# Run ON node9900 (detached). Usage: NP=8 EASY_MB=2048 HARD_SEGS=2000 orcd_mini_ior_node9900.sh; deletes its files afterwards.
# ior on the orcd-mini node9900 mount, 8 local ranks (geometry from
# tests/perf/io500/run.sh: easy -t 1m file-per-proc, hard -t/-b 47008 shared).
. /usr/mpi/gcc/openmpi-5.0.10rc2.2605121430/bin/mpivars.sh; set -u
IOR=$HOME/orcd/scratch/efs-io500/io500/bin/ior; NP=${NP:-8}
D=/data1/efs/orcd-mini-mnt/qcat-ior-$(date +%s); O=/data1/efs/orcd-mini-client/prof/qcat/ior
mkdir -p $D/easy $D/hard $O
R="mpirun -np $NP --host localhost:$NP"
echo "start $(date -u +%FT%TZ) np=$NP dir=$D" > $O/summary.txt
timeout 1200 $R $IOR -a POSIX -F -e -w -r -t 1m -b ${EASY_MB:-2048}m -o $D/easy/f > $O/easy.txt 2>&1; echo "easy rc=$?" >> $O/summary.txt
timeout 1200 $R $IOR -a POSIX -e -w -r -t 47008 -b 47008 -s ${HARD_SEGS:-2000} -o $D/hard/file > $O/hard.txt 2>&1; echo "hard rc=$?" >> $O/summary.txt
grep -E "^(write|read) |Max Write|Max Read" $O/easy.txt | sed "s/^/easy: /" >> $O/summary.txt
grep -E "^(write|read) |Max Write|Max Read" $O/hard.txt | sed "s/^/hard: /" >> $O/summary.txt
rm -rf $D; echo "done $(date -u +%FT%TZ)" >> $O/summary.txt
