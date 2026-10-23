#!/bin/bash
# rejoin_fsync_repro.sh RELEASE_DIR LABEL [PORT]
#
# Does a client fsync fail right after a storage member restarts? Private
# 4-node TCP cluster (ports PORT..PORT+3, default 22410) on 127.0.0.1 with
# direct I/O, stores under /tmp/efs-rejoin/LABEL, one efs-fuse mount. Never
# touches orcd-mini or w100.
#   1. create+fsync 20 files (warm-up);
#   2. SIGTERM node 2, wait for exit, create 20 files, keep it down DOWN_S
#      seconds (default 20);
#   3. restart node 2 (same args) and immediately create+write+fsync a new
#      file every 0.2 s for 40 s.
# Prints each failure with its offset from the restart, and the totals.
set -u
REL=$1; L=$2; PORT=${3:-22410}
D=/tmp/efs-rejoin/$L
rm -rf "$D"; mkdir -p "$D"/{n1,n2,n3,n4,mnt,logs}
FUSE_LIBDIR=${FUSE_LIBDIR:-/data1/efs/deps/libfuse/lib64}
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export EFS_TRANSPORT=tcp
# D39: the configured member list (members 1-4 at PORT..PORT+3).
CONF="$D/cluster.conf"; : > "$CONF"
for i in 1 2 3 4; do echo "$i 127.0.0.1:$((PORT+i-1))" >> "$CONF"; done
export EFS_CLUSTER_CONF="$CONF"
start() {  # node index 1-4
  local i=$1
  nohup "$REL/efsd" --cluster "$CONF" --addr 127.0.0.1 --port $((PORT+i-1)) --storage "$D/n$i" \
    --quota 1G --direct-io >> "$D/logs/n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"
}
cleanup() {
  "$REL/scripts/client.sh" stop --force-discard "$D/mnt" > /dev/null 2>&1
  for i in 1 2 3 4; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  sleep 3; for i in 1 2 3 4; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
}
trap cleanup EXIT
for i in 1 2 3 4; do
  start $i
  for t in $(seq 60); do grep -q listening "$D/logs/n$i.log" && break; sleep 0.5; done
done
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs 127.0.0.1:$PORT 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" 127.0.0.1:$PORT "$D/mnt" > "$D/logs/client.out" 2>&1
findmnt -rn "$D/mnt" > /dev/null || { echo "$L: mount failed"; tail -5 "$D/logs/client.out"; exit 1; }
echo "== $L $("$REL/efsd" --version | head -1)"
python3 - "$D" "$REL" "$PORT" <<'EOF'
import os,sys,time,subprocess,signal
d,rel,port=sys.argv[1],sys.argv[2],int(sys.argv[3]); m=d+'/mnt'
def create(name):
    try:
        with open(f'{m}/{name}','xb',buffering=0) as f:
            f.write(b'rejoin payload '*400); os.fsync(f.fileno())
        return None
    except OSError as e: return e.errno
for i in range(20): assert create(f'warm{i}') is None, 'warm-up failed'
pid=int(open(f'{d}/logs/n2.pid').read()); os.kill(pid,signal.SIGTERM)
for _ in range(200):
    try: os.kill(pid,0); time.sleep(0.1)
    except ProcessLookupError: break
down=[create(f'down{i}') for i in range(20)]; time.sleep(float(os.environ.get('DOWN_S','20')))
subprocess.run(['bash','-c',f'cd {d}; nohup {rel}/efsd --cluster {d}/cluster.conf --addr 127.0.0.1 --port {port+1} --storage {d}/n2 --quota 1G --direct-io >> {d}/logs/n2.log 2>&1 & echo $! > {d}/logs/n2.pid'],check=True)
t0=time.time(); fails=[]; n=0
while time.time()-t0<40:
    rc=create(f'after{n}'); n+=1
    if rc is not None: fails.append((round(time.time()-t0,2),rc))
    time.sleep(0.2)
print('  down-phase creates: %d ok of %d'%(sum(x is None for x in down),len(down)))
print('  after restart: %d creates, %d failures'%(n,len(fails)))
for t,e in fails[:20]: print('    +%.2fs errno=%d'%(t,e))
EOF
grep -h "fsync:\|network error" "$D/logs/fuse.log" | head -5 | cut -c1-160
