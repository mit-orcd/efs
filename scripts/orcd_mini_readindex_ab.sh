#!/bin/bash
# orcd_mini_readindex_ab.sh RELEASE_DIR LABEL [LOAD_SECONDS]
#
# ReadIndex lock-contention A/B on a private 3-daemon RDMA cluster on
# node9901 (ports 17631-17633, scratch stores under /data1/efs/prof/ab).
# Never touches orcd-mini (17432-17434) or w100 (27432/27433).
#
# Phases, each on a fresh cluster built from RELEASE_DIR:
#   1. idle probe: 200 x (create, stat, unlink) and 200 negative lookups;
#   2. loaded: 16 writer threads creating 256 B files (tiny_files pattern),
#      one reader re-reading a 2,000-file set, one crawler (walk + lstat),
#      while the same probe runs in the foreground; perf -e cs -g samples
#      all three efsd for PERF_SECONDS (default 10) in the middle.
# Prints probe percentiles, the load's files/s and the context-switch
# share that waits on a mutex (__lll_lock_wait) by caller. With
# DWARF_SECONDS>0 (default 3) it then takes a DWARF-unwound capture of the
# group-0 leader (perf -e cs --call-graph dwarf) and splits its lock waits
# into condvar wake-up re-acquisition (__pthread_mutex_cond_lock, i.e.
# cv_mu in host_waiter_sleep) and plain pthread_mutex_lock, by caller.
# Raw perf data stays under $AB (outside Git); the summary is on stdout.
set -u
REL=$1; L=$2; LOAD=${3:-40}; PERF_SECONDS=${PERF_SECONDS:-10}; DWARF_SECONDS=${DWARF_SECONDS:-3}
AB=/data1/efs/prof/ab/$L
rm -rf "$AB"; mkdir -p "$AB"/{n1,n2,n3,logs,mnt,perf}
# D39: the configured member list; the endpoint selects each member and
# efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
CONF="$AB/cluster.conf"; : > "$CONF"
for i in 1 2 3; do echo "$i 10.1.99.1:$((17630+i))" >> "$CONF"; done
export EFS_CLUSTER_CONF="$CONF"
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
for i in 1 2 3; do
  nohup "$REL/efsd" --cluster "$CONF" --addr 10.1.99.1 --port $((17630+i)) --storage "$AB/n$i" \
    --quota 30G --direct-io > "$AB/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$AB/logs/n$i.pid"
  for t in $(seq 60); do grep -q listening "$AB/logs/efsd-n$i.log" && break; sleep 0.5; done
done
cleanup() {
  "$REL/scripts/client.sh" stop "$AB/mnt" > /dev/null 2>&1
  for i in 1 2 3; do kill "$(cat "$AB/logs/n$i.pid")" 2>/dev/null; done
  sleep 3; for i in 1 2 3; do kill -9 "$(cat "$AB/logs/n$i.pid")" 2>/dev/null; done
  rm -rf "$AB"/n1 "$AB"/n2 "$AB"/n3
}
trap cleanup EXIT
for t in $(seq 30); do
  up=$("$REL/efs-mgmt" status 10.1.99.1:17631 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
  [ "$up" -eq 3 ] && break; sleep 1
done
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs 10.1.99.1:17631 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
EFS_FUSE_LOG="$AB/logs/fuse.log" "$REL/scripts/client.sh" 10.1.99.1:17631 "$AB/mnt" > "$AB/logs/client.out" 2>&1
findmnt -rn "$AB/mnt" > /dev/null || { echo "$L: mount failed"; tail -5 "$AB/logs/client.out"; exit 1; }
echo "== $L $("$REL/efsd" --version | head -1)"

cat > "$AB/probe.py" <<'EOF'
import os,sys,time,statistics as st
d=sys.argv[1]; n=int(sys.argv[2]); os.makedirs(d,exist_ok=True)
t={k:[] for k in ("create","stat","lookup_neg","unlink")}
blk=os.urandom(256)
for i in range(n):
    p=f"{d}/f{i:05d}"
    a=time.perf_counter(); fd=os.open(p,os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o644); b=time.perf_counter()
    os.write(fd,blk); os.close(fd); t["create"].append(b-a)
time.sleep(0.1)  # past the client's 50 ms lookup-row reuse
for i in range(n):
    p=f"{d}/f{i:05d}"
    a=time.perf_counter(); os.stat(p); t["stat"].append(time.perf_counter()-a)
    a=time.perf_counter()
    try: os.stat(f"{d}/missing{i:05d}")
    except FileNotFoundError: pass
    t["lookup_neg"].append(time.perf_counter()-a)
for i in range(n):
    p=f"{d}/f{i:05d}"; a=time.perf_counter(); os.unlink(p); t["unlink"].append(time.perf_counter()-a)
os.rmdir(d)
for k,v in t.items():
    v=sorted(v); print("  %-10s p50=%7.2f p90=%7.2f p99=%7.2f max=%7.2f ms"%(k,1e3*v[len(v)//2],1e3*v[int(len(v)*.9)],1e3*v[int(len(v)*.99)],1e3*v[-1]))
EOF
cat > "$AB/load.py" <<'EOF'
import os,sys,time,threading,hashlib
d=sys.argv[1]; dur=float(sys.argv[2]); W=16; blk=b"x"*256
os.makedirs(f"{d}/read"); stop=time.time()+dur; n=[0]*W; rd=[0]; cr=[0]
for i in range(2000):
    with open(f"{d}/read/f{i}","xb") as f: f.write(blk)
def writer(t):
    p=f"{d}/w{t}"; os.makedirs(p); i=0
    while time.time()<stop:
        with open(f"{p}/f{i}","xb") as f: f.write(blk)
        i+=1
    n[t]=i
def reader():
    while time.time()<stop:
        for i in range(2000):
            with open(f"{d}/read/f{i}","rb") as f: hashlib.md5(f.read()).digest()
            rd[0]+=1
            if time.time()>=stop: return
def crawler():
    while time.time()<stop:
        for root,dirs,files in os.walk(d):
            for x in files:
                try: os.lstat(os.path.join(root,x)); cr[0]+=1
                except FileNotFoundError: pass
                if time.time()>=stop: return
th=[threading.Thread(target=writer,args=(t,)) for t in range(W)]
th+=[threading.Thread(target=reader),threading.Thread(target=crawler)]
a=time.time(); [x.start() for x in th]; [x.join() for x in th]; e=time.time()-a
print("  load: %d creates by %d writers in %.1fs = %.0f files/s; reader %d opens, crawler %d lstats"%(sum(n),W,e,sum(n)/e,rd[0],cr[0]))
EOF
cat > "$AB/cs_lock.py" <<'EOF'
# perf script -> share of context switches parked in a mutex wait, by the
# first two frames above the pthread lock call.
import sys,collections
tot=0; lock=0; ri=0; by=collections.Counter(); st=[]
# Fresh-read admission/begin callers (LOOKUP pair, GETATTR lanes, scalar).
ri_frames=("server_raft_host_lookup","host_read_index","host_read_inode_lanes_reuse")
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","[unknown]")
def flush():
    global tot,lock,ri
    if not st: return
    tot+=1
    w=[i for i,f in enumerate(st) if "lll_lock_wait" in f]
    if w:
        lock+=1
        callers=[f for f in st[w[0]+1:] if not f.startswith(libc)]
        by[" <- ".join(callers[:2]) or "?"]+=1
        if callers and callers[0].startswith(ri_frames): ri+=1
for l in sys.stdin:
    l=l.rstrip()
    if not l: flush(); st=[]; continue
    if not l.startswith(("\t"," ")): continue
    p=l.split()
    if len(p)>1: st.append(p[1].split("+")[0])
flush()
print("  cs switches %d, mutex waits %d (%.1f%%), ReadIndex-caller waits %d (%.1f%%)"%(
    tot,lock,100.0*lock/max(tot,1),ri,100.0*ri/max(tot,1)))
for k,v in by.most_common(10): print("    %5.1f%% of switches  %s"%(100.0*v/max(tot,1),k[:160]))
EOF

cat > "$AB/cs_dwarf.py" <<'EOF'
# perf script (DWARF) -> lock-wait switches split into condvar re-acquire
# vs plain lock, top sites by the first three non-libc callers.
import sys,collections
lw=0; cond=0; by=collections.Counter(); st=[]
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","__GI_","[unknown]","syscall")
def flush():
    global lw,cond
    w=[i for i,f in enumerate(st) if "lll_lock_wait" in f]
    if not w: return
    lw+=1
    c=len(st)>w[0]+1 and "__pthread_mutex_cond_lock" in st[w[0]+1]
    cond+=c
    callers=[f for f in st[w[0]+1:] if not f.startswith(libc)]
    by[("cond-reacquire " if c else "lock ")+" <- ".join(callers[:3])]+=1
for l in sys.stdin:
    l=l.rstrip()
    if not l: flush(); st=[]; continue
    if not l.startswith(("\t"," ")): continue
    p=l.split()
    if len(p)>1: st.append(p[1].split("+")[0])
flush()
print("  leader lock-wait switches %d, cond re-acquire %d (%.1f%%)"%(lw,cond,100.0*cond/max(lw,1)))
for k,v in by.most_common(12): print("    %5.1f%%  %s"%(100.0*v/max(lw,1),k[:170]))
EOF

echo "-- idle probe"
python3 "$AB/probe.py" "$AB/mnt/idle" 200
echo "-- loaded (${LOAD}s: 16 writers + reader + crawler)"
python3 "$AB/load.py" "$AB/mnt/load" "$LOAD" > "$AB/logs/load.out" 2>&1 &
LP=$!
sleep $((LOAD/4))
pids=$(for i in 1 2 3; do cat "$AB/logs/n$i.pid"; done | paste -sd,)
perf record -e cs -g -p "$pids" -o "$AB/perf/cs.data" -- sleep "$PERF_SECONDS" > /dev/null 2>&1 &
PP=$!
python3 "$AB/probe.py" "$AB/mnt/probe" 200
wait $PP
if [ "$DWARF_SECONDS" -gt 0 ]; then
  # raft-status leader= is a 0-based peer index; ask each node for its role.
  lid=1
  for i in 1 2 3; do
    "$REL/efs-mgmt" raft-status 10.1.99.1:$((17630+i)) 2>/dev/null | grep -q '^  group 0 .*role=LEADER' && lid=$i
  done
  perf record -e cs --call-graph dwarf,16384 -p "$(cat "$AB/logs/n$lid.pid")" -o "$AB/perf/cs-dwarf.data" \
    -- sleep "$DWARF_SECONDS" > /dev/null 2>&1
  DW="leader n$lid"
fi
wait $LP; cat "$AB/logs/load.out"
perf script -i "$AB/perf/cs.data" 2>/dev/null | python3 "$AB/cs_lock.py"
grep -h "read-sleep" "$AB"/logs/efsd-n*.log | wc -l | sed 's/^/  read-sleep(>20ms) lines: /'
if [ "$DWARF_SECONDS" -gt 0 ]; then
  echo "  DWARF capture: $DW, ${DWARF_SECONDS}s"
  perf script -i "$AB/perf/cs-dwarf.data" 2>/dev/null | python3 "$AB/cs_dwarf.py"
fi
