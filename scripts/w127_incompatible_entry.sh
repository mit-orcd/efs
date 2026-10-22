#!/bin/bash
# w127_incompatible_entry.sh TREE [PORT]
#
# W127/W66 B1-B4 gate on a private 3-node efsd set (TCP by default; set
# EFS_TRANSPORT=rdma and EFS_RDMA_DEV for RDMA; ports PORT..PORT+2,
# default 17631; check `ss -ltn` first). TREE is a built, disposable tree;
# stores go to TREE/w127 and the daemons are killed on exit. Never point it
# at a deployed tree or a running cluster.
#
# A committed metadata entry with a tag this build does not know (200) is
# submitted to group 0 through the ordinary proposal RPC. Expected:
#   - every group-0 replica logs one INCOMPATIBLE diagnostic (group, index,
#     term, tag) and its applied index stays below that entry;
#   - group 0 has no leader afterwards and refuses proposals;
#   - group 2 keeps its leader and stays applied to its commit index;
#   - after a restart of all nodes the same holds (sticky, never skipped).
# Prints PASS/FAIL per check and exits nonzero on any failure.
set -u
T=$(cd "$1" && pwd -P); P=${2:-17631}; D=$T/w127
export EFS_TRANSPORT=${EFS_TRANSPORT:-tcp} EFS_MD_RAFT_N=3
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
start() {
  for i in 1 2 3; do
    j=""; [ $i -gt 1 ] && j="--join 127.0.0.1:$P"
    (cd "$T" || exit; nohup ./efsd --node-id $i --addr 127.0.0.1 --port $((P+i-1)) --storage "$D/n$i" --quota 1G $j \
      >> "$D/logs/n$i.log" 2>&1 & echo $! > "$D/logs/n$i.pid")
    for t in $(seq 60); do tail -n 40 "$D/logs/n$i.log" | grep -q "listening\|refusing startup" && break; sleep 0.5; done
    tail -n 40 "$D/logs/n$i.log" | grep -q "refusing startup" && { echo "FAIL n$i did not start"; fail=1; }
  done
}
stop() { for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid")" 2>/dev/null; done
         for t in $(seq 30); do alive=0; for i in 1 2 3; do kill -0 "$(cat "$D/logs/n$i.pid")" 2>/dev/null && alive=1; done
           [ $alive = 0 ] && return; sleep 1; done
         for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid")" 2>/dev/null; done; sleep 2; }
trap stop EXIT
rm -rf "$D"; mkdir -p "$D"/{n1,n2,n3,logs}
start
for t in $(seq 40); do "$T/efs-mgmt" raft-mkfs 127.0.0.1:$P 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
status() { "$T/efs-mgmt" raft-status 127.0.0.1:$((P+$1-1)) 2>/dev/null | grep "group $2 "; }
field() { sed -n "s/.* $1=\([-0-9]*\).*/\1/p"; }
# The unknown-tag entry: [group 0][tag 200][payload].
reply=$(python3 - "$P" <<'PY'
import socket,struct,sys
body=bytes([0,200,1,2,3,4])
s=socket.create_connection(('127.0.0.1',int(sys.argv[1])),60)
# W133: declare the protocol (CLIENT_HELLO 165, version 3) before the proposal.
s.sendall(struct.pack('>IB',17,165)+b'EFSP'+struct.pack('=IQ',3,0))
hello=b''
while len(hello)<13:hello+=s.recv(13-len(hello))
assert hello[4]==166 and struct.unpack('=iI',hello[5:13])==(0,3),hello.hex()
s.sendall(struct.pack('>IB',len(body)+1,95)+body)
n=struct.unpack('>I',s.recv(4))[0];data=b''
while len(data)<n:data+=s.recv(n-len(data))
rc,hint,index,salt,term=struct.unpack('=iiQQQ',data[1:33])
print(rc,index,term)
PY
)
echo "submit: rc index term = $reply"
index=$(echo "$reply" | cut -d' ' -f2)
sleep 4
for i in 1 2 3; do
  check "n$i logs the incompatible entry" "grep -q 'group=0 INCOMPATIBLE entry index=$index .*tag=200' '$D/logs/n$i.log'"
  a=$(status $i 0 | field applied)
  check "n$i group 0 applied ($a) stays below index $index" "[ -n '$a' ] && [ '$a' -lt '$index' ]"
done
check "group 0 has no leader" "! (for i in 1 2 3; do status \$i 0; done | grep -q 'role=LEADER')"
again=$("$T/efs-mgmt" raft-mkfs 127.0.0.1:$P 2>&1 | head -2)  # MKFS is a group-0 entry
check "group 0 refuses a new proposal" "! echo '$again' | grep -q 'raft-mkfs rc=0 '"
check "group 2 keeps a leader" "for i in 1 2 3; do status \$i 2; done | grep -q 'role=LEADER'"
for i in 1 2 3; do
  c=$(status $i 2 | field commit); a=$(status $i 2 | field applied)
  check "n$i group 2 applied=commit ($a/$c)" "[ -n '$a' ] && [ '$a' = '$c' ]"
done
echo "== restart"
stop; for i in 1 2 3; do echo "--- restart" >> "$D/logs/n$i.log"; done
start; sleep 6
for i in 1 2 3; do
  check "n$i refuses the same entry after restart" "sed -n '/--- restart/,\$p' '$D/logs/n$i.log' | grep -q 'group=0 INCOMPATIBLE entry index=$index .*tag=200'"
  a=$(status $i 0 | field applied)
  check "n$i group 0 applied ($a) still below $index" "[ -n '$a' ] && [ '$a' -lt '$index' ]"
done
check "group 0 still has no leader" "! (for i in 1 2 3; do status \$i 0; done | grep -q 'role=LEADER')"
[ $fail = 0 ] && echo "W127 incompatible entry: PASS" || echo "W127 incompatible entry: FAIL"
exit $fail
