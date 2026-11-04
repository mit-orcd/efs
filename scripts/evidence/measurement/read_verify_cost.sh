#!/bin/bash
# read_verify_cost.sh run|stop REL DIR [PORT] [MIB] [ROUNDS]
#
# D42 READ_VERIFY: client CPU cost of hashing every chunk fragment on GET
# receive. Private 3-daemon TCP cluster on ONE host: ports PORT+1..PORT+3
# (default 18801-18803), stores DIR/n{1,2,3}, one efs-fuse mount at DIR/mnt.
# REL is a build tree (efsd, efs-fuse, efs-mgmt, libefs.a, scripts/client.sh).
# Run it once with a build that hashes (D42 and later) and once with one that
# does not (devel 55ad94072, EFS_READ_VERIFY unset), on an idle host.
#
# run: format, write one MIB MiB random file (default 2048), then ROUNDS
# (default 3) cold reads: remount (empty client read cache), dd the file to
# /dev/null with 1 MiB reads, check its sha256 once. Per round it prints the
# efs-fuse CPU seconds (utime+stime from /proc) and the wall time. Then a
# single-thread efs_hash microbench over 64 KiB fragments built from
# REL/libefs.a. Raw logs stay in DIR/logs (outside Git). Never touches the
# live clusters (17432-17435, /data1/efs). Cleanup: `stop`, then delete DIR.
set -u
cmd=$1; REL=$2; D=$3; PORT=${4:-18800}; MIB=${5:-2048}; ROUNDS=${6:-3}
ADDR=127.0.0.1
export EFS_TRANSPORT=tcp
start_node() {
  local i=$1
  local ep=(--join "$ADDR:$((PORT+1))" --listen "$ADDR:$((PORT+i))")
  [ "$i" = 1 ] && ep=("$ADDR:$((PORT+1))")
  nohup "$REL/efsd" "${ep[@]}" "$D/n$i" --quota 16G --direct-io --discovery-port 0 \
    >> "$D/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"
}
# Joiners serve only once all three formed: start them, then wait for each.
wait_node() {
  local i=$1
  for _ in $(seq 120); do grep -q listening "$D/logs/efsd-n$i.log" && return 0
    kill -0 "$(cat "$D/logs/n$i.pid")" 2>/dev/null || return 1; sleep 0.5; done
  return 1
}
stop_nodes() {
  for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  for _ in $(seq 40); do
    alive=0; for i in 1 2 3; do kill -0 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null && alive=1; done
    [ $alive = 0 ] && return; sleep 0.5; done
  for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
}
mount_client() {
  : > "$D/logs/fuse.log"
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" "$ADDR:$((PORT+1))" "$D/mnt" \
    >> "$D/logs/client.out" 2>&1 && findmnt -rn "$D/mnt" > /dev/null
}
umount_client() { "$REL/scripts/client.sh" stop "$D/mnt" >> "$D/logs/client.out" 2>&1; }
fuse_pid() { grep -o 'efs-fuse daemon pid=[0-9]*' "$D/logs/fuse.log" | tail -1 | cut -d= -f2; }
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat"; }

case $cmd in
run)
  for i in 1 2 3; do ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }; done
  [ -e "$D/n1" ] && { echo "$D not fresh"; exit 1; }
  mkdir -p "$D"/{n1,n2,n3,logs,mnt}
  echo "build $("$REL/efsd" --version 2>&1 | head -1) transport=tcp direct=1 file=${MIB}MiB rounds=$ROUNDS"
  env | grep '^EFS_' | sort
  start_node 1; wait_node 1 || { echo "FAIL start n1"; exit 1; }
  start_node 2; start_node 3
  for i in 2 3; do wait_node $i || { echo "FAIL start n$i"; exit 1; }; done
  for _ in $(seq 60); do "$REL/efs-mgmt" status "$ADDR:$((PORT+1))" 2>/dev/null | grep -q 'Cluster nodes (3)' && break; sleep 1; done
  mount_client || { echo "FAIL mount"; tail -5 "$D/logs/client.out"; exit 1; }
  head -c $((MIB<<20)) /dev/urandom > "$D/logs/big.src"
  want=$(sha256sum < "$D/logs/big.src" | cut -c1-64)
  cp "$D/logs/big.src" "$D/mnt/big" && sync "$D/mnt/big"
  rm -f "$D/logs/big.src"
  hz=$(getconf CLK_TCK); fails=0
  for r in $(seq "$ROUNDS"); do
    umount_client; mount_client || { echo "FAIL remount"; exit 1; }
    pid=$(fuse_pid); c0=$(cpu_ticks "$pid"); t0=$(date +%s.%N)
    if [ "$r" = 1 ]; then got=$(dd if="$D/mnt/big" bs=1M 2>/dev/null | sha256sum | cut -c1-64)
    else dd if="$D/mnt/big" of=/dev/null bs=1M 2>/dev/null; got=$want; fi
    t1=$(date +%s.%N); c1=$(cpu_ticks "$pid")
    [ "$got" = "$want" ] || { echo "FAIL round $r sha256 $got"; fails=$((fails+1)); }
    awk -v r="$r" -v c="$((c1-c0))" -v hz="$hz" -v w="$(echo "$t1 - $t0" | bc)" -v m="$MIB" \
      'BEGIN{printf "round %d fuse_cpu_s %.2f wall_s %.2f MiB/s %.0f cpu_s_per_GiB %.3f\n", r, c/hz, w, m/w, c/hz*1024/m}'
  done
  umount_client; stop_nodes
  # Hash microbench: the per-fragment work the client adds.
  cat > "$D/logs/hash_bench.c" <<'EOC'
#include "efs/checksum.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(void)
{
    enum { FL = 64 * 1024, N = 32768 };          /* 2 GiB of fragments */
    unsigned char *b = malloc(FL), sum[EFS_HASH_SIZE];
    for (int i = 0; i < FL; i++) b[i] = (unsigned char)(i * 131 + 7);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) { b[i % FL] ^= (unsigned char)i; efs_hash(b, FL, sum); }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("efs_hash 64KiB fragments: %.1f us each, %.2f GiB/s, %.3f cpu_s_per_GiB (sum %02x)\n",
           s / N * 1e6, (double)N * FL / s / (1 << 30), s / ((double)N * FL / (1 << 30)), sum[0]);
    return 0;
}
EOC
  cc -O3 -march=native -I"$REL/include" "$D/logs/hash_bench.c" "$REL/libefs.a" -lpthread -lm -ldl -libverbs \
    -o "$D/logs/hash_bench" && "$D/logs/hash_bench"
  echo "RESULT fails=$fails"
  [ $fails = 0 ]
  ;;
stop)
  umount_client 2>/dev/null; "$REL/scripts/client.sh" stop --force-discard "$D/mnt" > /dev/null 2>&1
  stop_nodes; echo stopped
  ;;
*) echo "usage: $0 run|stop REL DIR [PORT] [MIB] [ROUNDS]"; exit 2 ;;
esac
