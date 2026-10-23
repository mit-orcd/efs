#!/bin/bash
# orcd_mini_stage_evict_ab.sh DAEMON_REL CLIENT_REL LABEL [LOAD_SECONDS]
#
# Client staging-evictor A/B on a private 3-daemon RDMA cluster on node9901
# (ports 17631-17633, scratch stores under ${AB_ROOT:-/tmp/efs-ab}/<label>;
# /data1 is below efsd's 20% free-space startup reserve once orcd-mini fills it).
# Never touches orcd-mini (17432-17434) or w100 (27432/27433).
#
# The daemons come from DAEMON_REL, the efs-fuse client (and client.sh)
# from CLIENT_REL, mounted with EFS_CLIENT_META_MB=${META_MB:-16} so the
# staging table is over its cap the whole run, as on orcd-mini under a
# crawler (QUOTA per daemon, default 30G; a 256 B file takes a 64 KiB
# allocation, so 30G holds about 491k files). Phases:
#   1. populate: 32 threads create ${POP_FILES:-200000} 256 B files in 400 dirs;
#   2. loaded (LOAD_SECONDS, default 60): one crawler (walk + lstat over the
#      tree and the writers' directories, looping), 16 writers creating
#      256 B files (INTERVAL=N prints rates every N s, to show whether they
#      decline as the client sees more inodes), and after 15 s the 200-file probe
#      (create/stat/negative lookup/unlink); efs-fuse per-thread CPU and a
#      10 s `perf -e cs -g` of efs-fuse (mutex waits by caller) mid-run.
# Prints the writers' files/s, crawler lstats/s, probe percentiles, the
# hottest efs-fuse thread and the client's mutex-wait share.
set -u
DREL=$1; CREL=$2; L=$3; LOAD=${4:-60}; META=${META_MB:-16}; POP=${POP_FILES:-200000}
AB=${AB_ROOT:-/tmp/efs-ab}/$L
rm -rf "$AB"; mkdir -p "$AB"/{n1,n2,n3,logs,mnt,perf}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
for i in 1 2 3; do
  join=""; [ $i -gt 1 ] && join="--join 10.1.99.1:17631"
  nohup "$DREL/efsd" --node-id $i --addr 10.1.99.1 --port $((17630+i)) --storage "$AB/n$i" \
    --quota ${QUOTA:-30G} --direct-io $join > "$AB/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$AB/logs/n$i.pid"
  for t in $(seq 60); do grep -q listening "$AB/logs/efsd-n$i.log" && break; sleep 0.5; done
done
cleanup() {
  "$CREL/scripts/client.sh" stop "$AB/mnt" > /dev/null 2>&1
  for i in 1 2 3; do kill "$(cat "$AB/logs/n$i.pid")" 2>/dev/null; done
  sleep 3; for i in 1 2 3; do kill -9 "$(cat "$AB/logs/n$i.pid")" 2>/dev/null; done
  rm -rf "$AB"/n1 "$AB"/n2 "$AB"/n3
}
trap cleanup EXIT
for t in $(seq 30); do
  up=$("$DREL/efs-mgmt" status 10.1.99.1:17631 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
  [ "$up" -eq 3 ] && break; sleep 1
done
for t in $(seq 30); do "$DREL/efs-mgmt" raft-mkfs 10.1.99.1:17631 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
EFS_CLIENT_META_MB=$META EFS_FUSE_LOG="$AB/logs/fuse.log" "$CREL/scripts/client.sh" 10.1.99.1:17631 "$AB/mnt" > "$AB/logs/client.out" 2>&1
findmnt -rn "$AB/mnt" > /dev/null || { echo "$L: mount failed"; tail -5 "$AB/logs/client.out"; exit 1; }
FP=$(pgrep -f "efs-fuse .*$AB/mnt" | head -1)
echo "== $L client $("$CREL/efs-fuse" --version 2>&1 | head -1) META_MB=$META"

cat > "$AB/pop.py" <<'EOF'
import os,sys,time
from concurrent.futures import ThreadPoolExecutor
d=sys.argv[1]; n=int(sys.argv[2]); D=400; blk=b"x"*256
def fill(k):
    p=f"{d}/d{k:03d}"; os.makedirs(p)
    for i in range(k, n, D):
        with open(f"{p}/f{i:07d}","xb") as f: f.write(blk)
a=time.time()
with ThreadPoolExecutor(32) as ex: list(ex.map(fill, range(D)))
print("  populate: %d files in %.0fs = %.0f files/s"%(n,time.time()-a,n/(time.time()-a)))
EOF
cat > "$AB/load.py" <<'EOF'
import os,sys,time,threading
d=sys.argv[1]; tree=sys.argv[2]; dur=float(sys.argv[3]); iv=float(os.environ.get("INTERVAL","0"))
W=16; blk=b"x"*256
stop=time.time()+dur; n=[0]*W; cr=[0]
def writer(t):
    p=f"{d}/w{t}"; os.makedirs(p); i=0
    while time.time()<stop:
        try:
            with open(f"{p}/f{i}","xb") as f: f.write(blk)
        except OSError as e:
            print("  writer %d stopped: %s"%(t,e),flush=True); return
        i+=1; n[t]=i
def crawler():
    # Walk the populated tree and the writers' growing directories.
    while time.time()<stop:
        for top in (tree, d):
            for r,ds,fs in os.walk(top):
                for x in fs:
                    try: os.lstat(os.path.join(r,x)); cr[0]+=1
                    except FileNotFoundError: pass
                    if time.time()>=stop: return
def report():
    a=time.time(); last=(a,0,0)
    while time.time()<stop:
        time.sleep(min(iv,max(0,stop-time.time())))
        t=time.time(); c=sum(n); l=cr[0]
        print("  t=%4.0fs creates/s=%5.0f lstats/s=%5.0f created=%d"%(t-a,(c-last[1])/(t-last[0]),(l-last[2])/(t-last[0]),c),flush=True)
        last=(t,c,l)
th=[threading.Thread(target=writer,args=(t,)) for t in range(W)]+[threading.Thread(target=crawler)]
if iv>0: th.append(threading.Thread(target=report))
a=time.time(); [x.start() for x in th]; [x.join() for x in th]; e=time.time()-a
print("  load: %d creates by %d writers in %.1fs = %.0f files/s; crawler %d lstats = %.0f/s"%(sum(n),W,e,sum(n)/e,cr[0],cr[0]/e))
EOF
cat > "$AB/probe.py" <<'EOF'
import os,sys,time
d=sys.argv[1]; n=int(sys.argv[2]); os.makedirs(d,exist_ok=True)
t={k:[] for k in ("create","stat","lookup_neg","unlink")}
blk=b"x"*256
for i in range(n):
    p=f"{d}/f{i:05d}"
    a=time.perf_counter(); fd=os.open(p,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o644); b=time.perf_counter()
    os.write(fd,blk); os.close(fd); t["create"].append(b-a)
time.sleep(0.1)
for i in range(n):
    p=f"{d}/f{i:05d}"
    a=time.perf_counter(); os.stat(p); t["stat"].append(time.perf_counter()-a)
    a=time.perf_counter()
    try: os.stat(f"{d}/missing{i:05d}")
    except FileNotFoundError: pass
    t["lookup_neg"].append(time.perf_counter()-a)
for i in range(n):
    p=f"{d}/f{i:05d}"; a=time.perf_counter(); os.unlink(p); t["unlink"].append(time.perf_counter()-a)
for k,v in t.items():
    v=sorted(v); print("  %-10s p50=%7.2f p90=%7.2f p99=%7.2f max=%7.2f ms"%(k,1e3*v[len(v)//2],1e3*v[int(len(v)*.9)],1e3*v[int(len(v)*.99)],1e3*v[-1]))
EOF
cat > "$AB/cs_client.py" <<'EOF'
import sys,collections
tot=0;lw=0;by=collections.Counter();st=[]
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","[unknown]")
def flush():
    global tot,lw
    if not st: return
    tot+=1
    w=[i for i,f in enumerate(st) if "lll_lock_wait" in f]
    if w:
        lw+=1; c=[f for f in st[w[0]+1:] if not f.startswith(libc)]; by[" <- ".join(c[:2]) or "?"]+=1
for l in sys.stdin:
    l=l.rstrip()
    if not l: flush(); st=[]; continue
    p=l.split(); st.append(p[-1] if len(p)>1 else "?")
flush()
print("  efs-fuse switches %d, mutex waits %d (%.1f%%)"%(tot,lw,100.0*lw/max(tot,1)))
for k,v in by.most_common(5): print("    %5.1f%% of lock waits  %s"%(100.0*v/max(lw,1),k[:140]))
EOF

python3 "$AB/pop.py" "$AB/mnt/tree" "$POP"
echo "-- loaded (${LOAD}s: 16 writers + crawler, probe at 15 s)"
python3 "$AB/load.py" "$AB/mnt/load" "$AB/mnt/tree" "$LOAD" > "$AB/logs/load.out" 2>&1 &
LP=$!
sleep 15
c0=$(awk '{print $14+$15}' /proc/$FP/stat)
perf record -e cs -g -p "$FP" -o "$AB/perf/cs-fuse.data" -- sleep 10 > /dev/null 2>&1 &
PP=$!
python3 "$AB/probe.py" "$AB/mnt/probe" 200
wait $PP
c1=$(awk '{print $14+$15}' /proc/$FP/stat)
echo "  efs-fuse CPU over the probe window: $(( (c1-c0) / ( $(getconf CLK_TCK) / 100 ) / 10 ))% of one core (approx)"
top -H -b -n1 -p "$FP" | sed -n 8,9p | awk '{print "  hottest efs-fuse thread: "$9"% CPU, "$11" total"}'
wait $LP; cat "$AB/logs/load.out"
perf script -i "$AB/perf/cs-fuse.data" -F ip,sym 2>/dev/null | python3 "$AB/cs_client.py"
grep -c "nothing evictable" "$AB/logs/fuse.log" | sed 's/^/  "nothing evictable" log lines: /'
