#!/bin/bash
# Sample efsd CPU (cores) and HCA TX (Gbps) over $1 seconds (default 3).
# RDMA traffic does not appear in /proc/net/dev (bypasses IPoIB), so read the
# HCA hardware counter port_xmit_data (units of 4 octets).
D=${1:-3}
DEV=${EFS_RDMA_DEV:-mlx5_2}
CNT=/sys/class/infiniband/$DEV/ports/1/counters/port_xmit_data
P=$(pgrep -x efsd | head -1)
[ -z "$P" ] && { echo "no efsd"; exit 1; }
u1=$(awk '{print $14+$15}' /proc/$P/stat)
t1=$(awk '{t=0; for(i=2;i<=8;i++)t+=$i; print t}' /proc/stat)
r1=$(cat "$CNT" 2>/dev/null || echo 0)
sleep "$D"
u2=$(awk '{print $14+$15}' /proc/$P/stat)
t2=$(awk '{t=0; for(i=2;i<=8;i++)t+=$i; print t}' /proc/stat)
r2=$(cat "$CNT" 2>/dev/null || echo 0)
NC=$(nproc)
awk -v a="$u1" -v b="$u2" -v c="$t1" -v d="$t2" -v e="$r1" -v f="$r2" -v nc="$NC" -v dt="$D" \
  'BEGIN { printf "efsd_cores=%.1f tx_gbps=%.1f\n", (b-a)/(d-c)*nc, 8*4*(f-e)/dt/1e9 }'
