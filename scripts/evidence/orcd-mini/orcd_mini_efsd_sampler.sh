#!/bin/bash
# Run ON node9901 (detached). Usage: orcd_mini_efsd_sampler.sh [SECONDS]; writes /data1/efs/prof/qcat-live/sampler.tsv
# 30 s samples of orcd-mini efsd CPU and GC counters for DUR seconds.
DUR=${1:-1800}; O=/data1/efs/prof/qcat-live/sampler.tsv; mkdir -p $(dirname $O)
P=$(for port in 17432 17433 17434; do pgrep -f "efsd --node-id .*--port $port"; done)
T=$(getconf CLK_TCK); prev=""; end=$(( $(date +%s) + DUR ))
echo -e "time\tcpu_n1\tcpu_n2\tcpu_n3\tremoved\tdata_usage\tpending_g0\treap_g0\tpending_g2\treap_g2" > $O
while [ $(date +%s) -lt $end ]; do
  cur=$(for p in $P; do awk '{print $14+$15}' /proc/$p/stat; done | paste -sd" "); now=$(date +%s.%N)
  g=$(/data1/efs/src/efs-mgmt gc-status 10.1.99.1:17432)
  rm=$(echo "$g" | grep -o "removed_fragments=[0-9]*" | cut -d= -f2); du=$(echo "$g" | grep -o "data_usage_bytes=[0-9]*" | cut -d= -f2)
  gr=$(echo "$g" | awk '/^group=/{split($3,a,"=");split($4,b,"=");printf "%s\t%s\t",a[2],b[2]}')
  if [ -n "$prev" ]; then
    c=$(paste <(echo $prev | tr " " "\n") <(echo $cur | tr " " "\n") | awk -v d=$(echo "$now-$pt"|bc) -v t=$T '{printf "%.0f\t",100*($2-$1)/t/d}')
    echo -e "$(date -u +%T)\t${c}${rm}\t${du}\t${gr}" >> $O
  fi
  prev=$cur; pt=$now; sleep 30
done
