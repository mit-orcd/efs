#!/bin/bash
# Run ON node9900 (detached, in screen): orcd-mini batch O ior runs through the
# RDMA mount /data1/efs/orcd-mini-mnt, 8 local ranks, geometry from
# tests/perf/io500/run.sh and scripts/evidence/orcd-mini/orcd_mini_ior_node9900.sh.
# Usage: orcd_mini_batch_o_ior.sh hard|easy REPS OUTDIR [TAG]
#   hard: one shared file, -t/-b 47008, 2000 segments per rank (ior hard, O1)
#   easy: file per process, -t 1m -b EASY_MB (default 2048m), write+read (O2)
# One ior per rep, each in a fresh directory deleted afterwards; OUTDIR must
# not exist. Exit status is the number of failed reps.
. /usr/mpi/gcc/openmpi-5.0.10rc2.2605121430/bin/mpivars.sh; set -u
MODE=$1; REPS=$2; O=$3; TAG=${4:-run}
[ -e "$O" ] && { echo "$O exists" >&2; exit 2; }
mkdir -p "$O"
IOR=$HOME/orcd/scratch/efs-io500/io500/bin/ior; NP=${NP:-8}
R="mpirun -np $NP --host localhost:$NP"
fail=0
for i in $(seq 1 "$REPS"); do
  D=/data1/efs/orcd-mini-mnt/bq-ior-$TAG-$MODE-$i-$(date +%s); mkdir -p "$D"
  t0=$(date +%s)
  if [ "$MODE" = hard ]; then
    timeout 1200 $R $IOR -a POSIX -e -w -r -t 47008 -b 47008 -s "${HARD_SEGS:-2000}" -o "$D/file" > "$O/$MODE-$i.txt" 2>&1
  else
    timeout 1200 $R $IOR -a POSIX -F -e -w -r -t 1m -b "${EASY_MB:-2048}m" -o "$D/f" > "$O/$MODE-$i.txt" 2>&1
  fi
  rc=$?; [ $rc -eq 0 ] || fail=$((fail+1))
  echo "$MODE rep=$i rc=$rc seconds=$(( $(date +%s) - t0 ))" >> "$O/summary.txt"
  grep -E "^(write|read) " "$O/$MODE-$i.txt" | sed "s/^/  /" >> "$O/summary.txt"
  rm -rf "$D"
done
echo "done fail=$fail $(date -u +%FT%TZ)" >> "$O/summary.txt"
exit $fail
