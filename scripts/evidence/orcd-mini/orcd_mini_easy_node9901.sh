#!/bin/bash
# Run ON node9901 (detached): K32 ior easy from the node9901 client mount
# (/data1/efs/mnt, the client of the Oct 22/25 measurements), Task EW.
# Usage: orcd_mini_easy_node9901.sh OUTDIR [REPS]
# First waits until each of n1-n3 uses < 0.2 core over two consecutive 60 s
# samples, at most 30 min (the cluster's own reclaim after a large delete
# would understate the rate; quiet.txt says whether it got quiet). Records
# per-daemon CPU and `efs-mgmt status` before and after, then REPS ior easy
# runs (8 ranks, file per process, 1 MiB x 2 GiB, OpenMPI 4.1.7 as on Oct 25).
. /usr/mpi/gcc/openmpi-4.1.7rc1/bin/mpivars.sh; set -u
O=$1; REPS=${2:-3}
[ -e "$O" ] && { echo "$O exists" >&2; exit 2; }
mkdir -p "$O"
IOR=$HOME/orcd/scratch/efs-io500/io500/bin/ior; M=/data1/efs/src/efs-mgmt
ticks() { awk '{print $14+$15}' "/proc/$(cat /data1/efs/logs/efsd-n$1.pid)/stat"; }
cpu60() { # prints "nN=cores" for n1..n4 over 60 s
  local -a a; local n
  for n in 1 2 3 4; do a[$n]=$(ticks $n); done; sleep 60
  for n in 1 2 3 4; do awk -v n=$n -v d=$(( $(ticks $n) - a[n] )) 'BEGIN{printf "n%s=%.2f ", n, d/6000}'; done; echo
}
snap() { echo "== $1 $(date -u +%T) load $(cut -d' ' -f1-3 /proc/loadavg)"; echo "cpu60 $(cpu60)"
  "$M" status 10.1.99.1:17432 2>&1 | grep -E 'Usable \(|Abandoned|Degraded|Cluster state'; }
end=$(( $(date +%s) + 1800 )); quiet=0
while [ "$(date +%s)" -lt "$end" ]; do
  s=$(cpu60); echo "$(date -u +%T) $s" >> "$O/wait.txt"
  if echo "$s" | awk '{for(i=1;i<=3;i++){split($i,v,"="); if (v[2]>=0.2) exit 1}}'; then
    quiet=$((quiet+1)); [ $quiet -ge 2 ] && break
  else quiet=0; fi
done
[ $quiet -ge 2 ] && echo "quiet: yes" > "$O/quiet.txt" || echo "quiet: no (30 min cap reached)" > "$O/quiet.txt"
snap before > "$O/before.txt"
for i in $(seq 1 "$REPS"); do
  D=/data1/efs/mnt/ew-easy-9901-$i-$(date +%s); mkdir -p "$D"
  timeout 1200 mpirun -np 8 --host localhost:8 $IOR -a POSIX -F -e -w -r -t 1m -b 2048m -o "$D/f" > "$O/easy-$i.txt" 2>&1
  echo "easy rep=$i rc=$? $(grep -a -E '^(write|read) ' "$O/easy-$i.txt" | head -2 | awk '{printf "%s=%s ", $1, $2}')" >> "$O/summary.txt"
  rm -rf "$D"
done
snap after > "$O/after.txt"
echo "done $(date -u +%FT%TZ)" >> "$O/summary.txt"
