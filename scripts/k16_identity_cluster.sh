#!/bin/bash
# k16_identity_cluster.sh run|stop REL DIR [PORT]
#
# K16 / W75 (I25) cluster gate for the identity-bound fragment record
# (fragment record format 2, docs/how-it-works/protocols/data.md).
# Private 3-daemon TCP cluster on ONE host: ports PORT+1..PORT+3 (default
# 18401-18403), stores DIR/n{1,2,3}, DIR/cluster.conf (D39), one efs-fuse mount at
# DIR/mnt. REL is a build tree (efsd, efs-fuse, efs-mgmt, scripts/client.sh).
# Never touches orcd-mini or w100. K16_DIRECT=1 (default) adds --direct-io.
#
# run: fresh format -> write -> cold restart -> read back -> corrupt on disk
# (object swap, rotted identity bytes, two bad fragments of one chunk) ->
# remount and read; then the startup refusal of an unmarked root holding
# data. Prints PASS/FAIL per check and exits nonzero on any FAIL.
# Raw logs stay in DIR/logs (outside Git). Cleanup: `stop`, then delete DIR.
set -u
cmd=$1; REL=$2; D=$3; PORT=${4:-18400}
ADDR=${K16_ADDR:-127.0.0.1}
export EFS_TRANSPORT=tcp
DIO=""; [ "${K16_DIRECT:-1}" = 1 ] && DIO=--direct-io
FAILS=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS+1)); fi; }

# D39: the configured member list; the endpoint selects each member and
# efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
export EFS_CLUSTER_CONF="$D/cluster.conf"
write_conf() {
  : > "$D/cluster.conf"
  for i in 1 2 3; do echo "$i $ADDR:$((PORT+i))" >> "$D/cluster.conf"; done
}

start_node() {
  local i=$1
  nohup "$REL/efsd" --cluster "$D/cluster.conf" --addr "$ADDR" --port $((PORT+i)) --storage "$D/n$i" \
    --quota 4G $DIO >> "$D/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"
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
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" "$ADDR:$((PORT+1))" "$D/mnt" \
    >> "$D/logs/client.out" 2>&1 && findmnt -rn "$D/mnt" > /dev/null
}
umount_client() { "$REL/scripts/client.sh" stop "$D/mnt" >> "$D/logs/client.out" 2>&1; }
# Fragment file of (ino, chunk, fragment) on whichever node holds it.
frag() {
  local ino=$1 ci=$2 fi=$3 s=""
  local g4=$((ino/1000000000000000000%10000)) g3=$((ino/1000000000000%10000)) g2=$((ino/100000000%10000))
  local g1=$((ino/10000%10000)) g0=$((ino%10000))
  s=$(printf '%04d/%04d/%04d/%04d/%04d' $g4 $g3 $g2 $g1 $g0)
  ls -t "$D"/n*/data/exports/1/"$s"/$((ci>>10))/"$ci.$fi".* 2>/dev/null | head -1 # newest generation
}
rec_off() { # byte offset of the record in a fragment file
  local sz; sz=$(stat -c %s "$1")
  if [ $(( (sz-128) % 4096 )) = 0 ]; then echo $((sz-128)); else echo $((sz-4096)); fi
}
refusals() { cat "$D"/logs/efsd-n*.log | grep -c "I25 get refused" ; }

case $cmd in
run)
  for i in 1 2 3; do ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }; done
  [ -e "$D/n1" ] && { echo "$D not fresh"; exit 1; }
  mkdir -p "$D"/{n1,n2,n3,logs,mnt}
  write_conf
  echo "build $("$REL/efsd" --version 2>&1 | head -1) direct=${K16_DIRECT:-1} transport=tcp"
  for i in 1 2 3; do start_node $i || { echo "FAIL start n$i"; exit 1; }; done
  for _ in $(seq 30); do "$REL/efs-mgmt" raft-mkfs "$ADDR:$((PORT+1))" 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  mount_client || { echo "FAIL mount"; tail -5 "$D/logs/client.out"; exit 1; }
  check "fresh roots marked root format 1" \
    '[ "$(od -An -tu1 -j8 -N1 "$D/n1/.efs-engine" | tr -d " ")" = 1 ]'
  # 1. write: 32 MiB random (256 x 128 KiB chunks) plus 64 small files.
  head -c $((32<<20)) /dev/urandom > "$D/logs/big.src"
  cp "$D/logs/big.src" "$D/mnt/big" && sync "$D/mnt/big"
  for k in $(seq 64); do head -c $((k*1000)) /dev/urandom > "$D/logs/s$k"; cp "$D/logs/s$k" "$D/mnt/s$k"; done
  sync -f "$D/mnt"
  want=$(sha256sum < "$D/logs/big.src" | cut -c1-64)
  ino=$(stat -c %i "$D/mnt/big"); echo "big ino=$ino sha256=$want"
  f0=$(frag "$ino" 0 0)
  check "fragment files carry the record (magic EFSFRAG)" \
    '[ -n "$f0" ] && [ "$(dd if="$f0" bs=1 skip=$(rec_off "$f0") count=7 2>/dev/null)" = EFSFRAG ]'
  # 2. cold restart and read back.
  umount_client; stop_nodes
  for i in 1 2 3; do start_node $i || { echo "FAIL restart n$i"; exit 1; }; done
  mount_client || { echo "FAIL remount"; exit 1; }
  check "cold restart: big file reads back identical" \
    '[ "$(sha256sum < "$D/mnt/big" | cut -c1-64)" = "$want" ]'
  bad=0; for k in $(seq 64); do cmp -s "$D/logs/s$k" "$D/mnt/s$k" || bad=$((bad+1)); done
  check "cold restart: 64 small files identical" '[ $bad = 0 ]'
  check "no refusals on a healthy store" '[ "$(refusals)" = 0 ]'
  umount_client
  # 3a. object swap: chunk 1's fragment 0 copied over chunk 0's fragment 0
  # (complete, self-consistent record of another object under this name).
  src=$(frag "$ino" 1 0); dst=$(frag "$ino" 0 0)
  echo "swap $src -> $dst"; cp "$dst" "$D/logs/swap.saved"
  cat "$src" > "$dst"
  # 3b. rotted identity bytes: flip one byte of chunk 3 fragment 1's object field.
  f31=$(frag "$ino" 3 1); off=$(( $(rec_off "$f31") + 24 ))
  echo "flip $f31 @$off"
  printf '\xff' | dd of="$f31" bs=1 seek=$off count=1 conv=notrunc 2>/dev/null
  mount_client || { echo "FAIL remount after corruption"; exit 1; }
  check "one refused fragment per chunk: reconstructed from two verified fragments" \
    '[ "$(sha256sum < "$D/mnt/big" | cut -c1-64)" = "$want" ]'
  grep -h "I25 get refused" "$D"/logs/efsd-n*.log | head -4
  check "server logged identity refusal (object swap)" \
    'grep -q "I25 get refused fragment $dst: fragment identity mismatch field=generation" "$D"/logs/efsd-n*.log'
  check "server logged bound-digest refusal (rotted identity bytes)" \
    'grep -q "I25 get refused fragment $f31: checksum mismatch" "$D"/logs/efsd-n*.log'
  check "client logged the refusals" 'grep -q "read: I25 node refused fragment ino=$ino" "$D/logs/fuse.log"'
  umount_client
  # 3c. two bad fragments of one chunk: never served, the read fails.
  f50=$(frag "$ino" 5 0); f52=$(frag "$ino" 5 2)
  cat "$(frag "$ino" 6 0)" > "$f50"; cat "$(frag "$ino" 6 2)" > "$f52"
  mount_client || { echo "FAIL remount 3c"; exit 1; }
  dd if="$D/mnt/big" of=/dev/null bs=128k skip=5 count=1 2> "$D/logs/dd5.err"; rc=$?
  check "two refused fragments in chunk 5: read fails (EIO), nothing served" \
    '[ $rc != 0 ] && grep -q "Input/output error" "$D/logs/dd5.err"'
  check "other chunks still read correctly" \
    '[ "$(dd if="$D/mnt/big" bs=128k skip=7 count=8 2>/dev/null | sha256sum | cut -c1-64)" = "$(dd if="$D/logs/big.src" bs=128k skip=7 count=8 2>/dev/null | sha256sum | cut -c1-64)" ]'
  umount_client; stop_nodes
  # 4. startup refusal: an unmarked root that already holds fragment data.
  mv "$D/n3/.efs-engine" "$D/logs/n3.efs-engine"
  start_node 3; sleep 1
  check "unmarked root with data refused at startup" \
    'grep -q "has fragment data but no root marker; its format is unknown and this build reads only root format 1" "$D/logs/efsd-n3.log" && ! kill -0 "$(cat "$D/logs/n3.pid")" 2>/dev/null'
  mv "$D/logs/n3.efs-engine" "$D/n3/.efs-engine"
  echo "refusals logged by servers: $(refusals)"
  echo "RESULT fails=$FAILS"
  [ $FAILS = 0 ]
  ;;
stop)
  umount_client 2>/dev/null; "$REL/scripts/client.sh" stop --force-discard "$D/mnt" > /dev/null 2>&1
  stop_nodes; echo stopped
  ;;
*) echo "usage: $0 run|stop REL DIR [PORT]"; exit 2 ;;
esac
