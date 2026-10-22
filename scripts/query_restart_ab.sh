#!/bin/bash
# Reproduces the startup .efs EBUSY window fixed in the commit that added
# this script: run once with an old release dir and once with a fixed tree.
# qstart.sh BIN LABEL: 3-node TCP cluster; populate, restart all daemons, then
# query a new directory from the first second; report rc timeline.
B=$1; L=$2; R=/tmp/qstart-$L; P=17760
for p in 17761 17762 17763; do ss -ltn | grep -q ":$p " && { echo "port $p busy"; exit 1; }; done
rm -rf $R; mkdir -p $R/n1 $R/n2 $R/n3 $R/mnt $R/logs
export EFS_MD_RAFT_N=3 EFS_TRANSPORT=tcp EFS_COMMANDS=1 EFS_COMMANDS_SERVER=1 EFS_EXPIRY=0
start(){ for i in 1 2 3; do j=""; [ $i -gt 1 ] && j="--join 127.0.0.1:$((P+1))"
  setsid $B/efsd --node-id $i --addr 127.0.0.1 --port $((P+i)) --storage $R/n$i --quota 8G --direct-io $j >> $R/logs/n$i.log 2>&1 < /dev/null &
  echo $! > $R/logs/n$i.pid; done
  for i in 1 2 3; do for t in $(seq 120); do [ $(grep -c listening $R/logs/n$i.log) -ge $1 ] && break; sleep 0.25; done; done; }
stop(){ $B/scripts/client.sh stop $R/mnt > /dev/null 2>&1; for i in 1 2 3; do kill $(cat $R/logs/n$i.pid); done; for t in $(seq 40); do pgrep -f "storage $R/" > /dev/null || break; sleep 0.25; done; }
start 1; for t in $(seq 30); do $B/efs-mgmt raft-mkfs 127.0.0.1:$((P+1)) 2>&1 | grep -q "rc=0" && break; sleep 1; done
EFS_FUSE_LOG=$R/logs/fuse.log $B/scripts/client.sh 127.0.0.1:$((P+1)) $R/mnt > /dev/null 2>&1
mkdir $R/mnt/old; for f in $(seq 200); do echo x > $R/mnt/old/f$f; done; sleep 65; stop
T0=$(date +%s.%N); start 2; EFS_FUSE_LOG=$R/logs/fuse2.log $B/scripts/client.sh 127.0.0.1:$((P+1)) $R/mnt > /dev/null 2>&1
d=$R/mnt/new; mkdir $d; echo x > $d/a; busy=0; ok=""; n75=0
for k in $(seq 150); do python3 $d/.efs/stats --json > /dev/null 2> $R/logs/err; rc=$?; t=$(echo "$(date +%s.%N) - $T0" | bc | cut -c1-5)
  case $rc in 0) ok=$t; break;; 75) n75=$((n75+1));; *) busy=$((busy+1)); grep -q busy $R/logs/err && last="EBUSY";; esac; sleep 0.5; done
echo "$L: hard-failures(rc!=0,75)=$busy not-indexed=$n75 first-success-at=${ok:-none}s"
grep -h "query-refresh\|query-publish" $R/logs/n*.log | awk -v s="$(date -u -d @${T0%.*} +%FT%T)" '$1>s' | cut -c1-120 | head -6
stop; rm -rf $R
