#!/bin/bash
# cleanup_smoke.sh TREE [PORT]: private 3-node TCP efsd smoke of the
# efs-mgmt and handler changes of the dead-code cleanup (ports PORT..PORT+2,
# default 17631; check `ss -ltn` first). TREE is a built, disposable
# git-archive tree; its stores go to TREE/smoke and the daemons are killed
# on exit. Never point it at a deployed tree or a running cluster.
# Linux build hosts only (node9901: export LD_LIBRARY_PATH for libfuse).
set -u
T=$(cd "$1" && pwd -P); D=$T/smoke; P=${2:-17631}
cd $T; rm -rf $D; mkdir -p $D/{n1,n2,n3,logs}
export EFS_TRANSPORT=tcp
for i in 1 2 3; do
  j=""; [ $i -gt 1 ] && j="--join 127.0.0.1:$P"
  nohup ./efsd --node-id $i --addr 127.0.0.1 --port $((P+i-1)) --storage $D/n$i --quota 1G $j >> $D/logs/n$i.log 2>&1 &
  echo $! > $D/logs/n$i.pid
  for t in $(seq 60); do grep -q listening $D/logs/n$i.log && break; sleep 0.5; done
done
cleanup(){ for i in 1 2 3; do kill $(cat $D/logs/n$i.pid) 2>/dev/null; done; sleep 2; for i in 1 2 3; do kill -9 $(cat $D/logs/n$i.pid) 2>/dev/null; done; }
trap cleanup EXIT
echo "== mkfs with a label (expect usage error)"; ./efs-mgmt raft-mkfs 127.0.0.1:$P label; echo "rc=$?"
for t in $(seq 40); do ./efs-mgmt raft-mkfs 127.0.0.1:$P 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
./efs-mgmt raft-mkfs 127.0.0.1:$P 2>&1 | grep -o 'raft-mkfs rc=[-0-9]*'
echo "== status"; ./efs-mgmt status 127.0.0.1:$P; echo "rc=$?"
echo "== io-stats"; ./efs-mgmt io-stats 127.0.0.1:$P; echo "rc=$?"
echo "== gc-status"; ./efs-mgmt gc-status 127.0.0.1:$P | head -3; echo "rc=${PIPESTATUS[0]}"
sleep 5
echo "== pack-compact 0"; ./efs-mgmt pack-compact 127.0.0.1:$P 0; echo "rc=$?"
echo "== expiry-policy positional form (expect usage error)"; ./efs-mgmt expiry-policy 127.0.0.1:$P set 1 1 0 86400000000000 604800000000000 3 2>&1 | head -2; echo "rc=${PIPESTATUS[0]}"
echo "== retired message 81 (expect closed connection, logged)"
python3 - $P <<'PY'
import socket,struct,sys
s=socket.create_connection(('127.0.0.1',int(sys.argv[1])));s.sendall(struct.pack('>IB',1,81));s.settimeout(5)
print('reply bytes:',len(s.recv(64)))
PY
grep -h "refused retired message" $D/logs/n1.log | head -2
