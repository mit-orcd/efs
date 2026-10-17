#!/bin/bash
# raft_host_mu_ab.sh RELEASE_DIR LABEL [LOAD_SECONDS]
#
# h->mu plan A/B (docs/status/raft-host-mu-plan-20261016.md) on a private
# 3-daemon RDMA cluster on node9901: ports $PORT+1..$PORT+3 (default
# 17651-17653), scratch stores under /tmp/efs-mu-ab/LABEL (/data1 is too
# full for efsd's free-space check). Never touches orcd-mini (17432-17434)
# or w100 (27432/27433).
#
# RELEASE_DIR holds efsd, efs-fuse, efs-mgmt and scripts/ of one build.
# Daemons run with EFS_WRITE_PROF=1 (needs a build with write_prof.h).
# Each run is a fresh cluster:
#   1. idle probe: 200 x (create, stat, negative lookup, unlink);
#   2. loaded: 16 writer threads creating 256 B files, one md5 reader over
#      a 2,000-file set and one crawler (walk + lstat) for LOAD_SECONDS,
#      with the same probe in the foreground.
# Prints: creates/s; efsd CPU per completed create (utime+stime of all
# three daemons and of the group-0 leader over the load window); probe
# p50/p95/p99; the leader's last write-phase-prof line (cumulative since
# start: n/sum_us/max_us/p50/p95/p99 per phase); and, with DWARF_SECONDS>0
# (default 3), the leader's lock-wait sites from perf -e cs --call-graph
# dwarf. Raw perf data and logs stay under /tmp/efs-mu-ab (outside Git).
set -u
REL=$1; L=$2; LOAD=${3:-40}; DWARF_SECONDS=${DWARF_SECONDS:-3}; PORT=${PORT:-17650}
AB=/tmp/efs-mu-ab/$L
rm -rf "$AB"; mkdir -p "$AB"/{n1,n2,n3,logs,mnt,perf}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 EFS_MD_RAFT_N=3 EFS_WRITE_PROF=1
# efs-fuse links the private libfuse (FUSE >= 3.12) used by orcd-mini builds.
FUSE_LIBDIR=${FUSE_LIBDIR:-/data1/efs/deps/libfuse/lib64}
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
for i in 1 2 3; do
  join=""; [ $i -gt 1 ] && join="--join 10.1.99.1:$((PORT+1))"
  nohup "$REL/efsd" --node-id $i --addr 10.1.99.1 --port $((PORT+i)) --storage "$AB/n$i" \
    --quota 30G --direct-io $join > "$AB/logs/efsd-n$i.log" 2>&1 &
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
  up=$("$REL/efs-mgmt" status 10.1.99.1:$((PORT+1)) 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
  [ "$up" -eq 3 ] && break; sleep 1
done
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs 10.1.99.1:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
EFS_FUSE_LOG="$AB/logs/fuse.log" "$REL/scripts/client.sh" 10.1.99.1:$((PORT+1)) "$AB/mnt" > "$AB/logs/client.out" 2>&1
findmnt -rn "$AB/mnt" > /dev/null || { echo "$L: mount failed"; tail -5 "$AB/logs/client.out"; exit 1; }
echo "== $L $("$REL/efsd" --version | head -1)"

cat > "$AB/probe.py" <<'EOF'
import os,sys,time
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
q=lambda v,f: 1e3*v[min(len(v)-1,int(len(v)*f))]
for k,v in t.items():
    v=sorted(v); print("  %-10s p50=%7.2f p95=%7.2f p99=%7.2f max=%7.2f ms"%(k,q(v,.5),q(v,.95),q(v,.99),1e3*v[-1]))
EOF
cat > "$AB/load.py" <<'EOF'
import os,sys,time,threading,hashlib
d=sys.argv[1]; dur=float(sys.argv[2]); mark=sys.argv[3]; W=16; blk=b"x"*256
os.makedirs(f"{d}/read"); n=[0]*W; rd=[0]; cr=[0]
for i in range(2000):
    with open(f"{d}/read/f{i}","xb") as f: f.write(blk)
open(mark+".start","w").close()      # CPU window starts after the read set
stop=time.time()+dur
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
open(mark+".end","w").close()
print("  load: %d creates by %d writers in %.1fs = %.0f files/s; reader %d opens, crawler %d lstats"%(sum(n),W,e,sum(n)/e,rd[0],cr[0]))
print("CREATES %d"%sum(n))
EOF
cat > "$AB/cs_dwarf.py" <<'EOF'
# perf script (DWARF) -> lock-wait switches split into condvar re-acquire
# vs plain lock, top sites by the first three non-libc callers.
import sys,collections
tot=0; lw=0; cond=0; by=collections.Counter(); st=[]
libc=("__lll","___pthread","__pthread","pthread_","lll_","futex","__futex","__GI_","[unknown]","syscall")
def flush():
    global tot,lw,cond
    if not st: return
    tot+=1
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
print("  leader switches %d, lock-wait %d (%.1f%%), cond re-acquire %d"%(tot,lw,100.0*lw/max(tot,1),cond))
for k,v in by.most_common(12): print("    %5.1f%% of lock waits  %s"%(100.0*v/max(lw,1),k[:170]))
EOF
cpu_ticks() { awk '{print $14+$15}' "/proc/$1/stat"; }

leader() {  # node (1-3) whose group $1 role is LEADER
  for i in 1 2 3; do
    "$REL/efs-mgmt" raft-status 10.1.99.1:$((PORT+i)) 2>/dev/null | grep -q "^  group $1 .*role=LEADER" && { echo $i; return; }
  done
  echo 1
}

echo "-- idle probe"
python3 "$AB/probe.py" "$AB/mnt/idle" 200
echo "-- loaded (${LOAD}s: 16 writers + reader + crawler)"
python3 "$AB/load.py" "$AB/mnt/load" "$LOAD" "$AB/logs/mark" > "$AB/logs/load.out" 2>&1 &
LP=$!
for t in $(seq 600); do [ -e "$AB/logs/mark.start" ] && break; sleep 0.1; done
lid=$(leader 0); lid2=$(leader 2)
declare -A c0
for i in 1 2 3; do c0[$i]=$(cpu_ticks "$(cat "$AB/logs/n$i.pid")"); done
sleep $((LOAD/4))
python3 "$AB/probe.py" "$AB/mnt/probe" 200
if [ "$DWARF_SECONDS" -gt 0 ]; then
  perf record -e cs --call-graph dwarf,16384 -p "$(cat "$AB/logs/n$lid.pid")" -o "$AB/perf/cs-dwarf.data" \
    -- sleep "$DWARF_SECONDS" > /dev/null 2>&1
fi
for t in $(seq $((LOAD*20+600))); do [ -e "$AB/logs/mark.end" ] && break; sleep 0.1; done
tot=0; ldr=0
for i in 1 2 3; do
  d=$(( $(cpu_ticks "$(cat "$AB/logs/n$i.pid")") - ${c0[$i]} )); tot=$((tot+d)); [ $i -eq $lid ] && ldr=$d
done
wait $LP; grep -v '^CREATES' "$AB/logs/load.out"
creates=$(awk '/^CREATES/{print $2}' "$AB/logs/load.out")
hz=$(getconf CLK_TCK)
python3 - "$tot" "$ldr" "$hz" "${creates:-0}" "$lid" "$lid2" <<'EOF'
import sys
tot,ldr,hz,n,l0,l2=map(int,sys.argv[1:])
print("  leaders: group0=n%d group2=n%d"%(l0,l2))
print("  efsd CPU over load: all %.1f s, leader %.1f s; per create: all %.1f us, leader %.1f us"%(
    tot/hz,ldr/hz,1e6*tot/hz/max(n,1),1e6*ldr/hz/max(n,1)))
EOF
echo "  leader write-phase-prof (cumulative):"
grep -h 'write-phase-prof' "$AB/logs/efsd-n$lid.log" | tail -1 | sed 's/^.*write-phase-prof/write-phase-prof/' | tr ' ' '\n' | sed 's/^/    /'
if [ "$DWARF_SECONDS" -gt 0 ]; then
  echo "  DWARF capture: leader n$lid, ${DWARF_SECONDS}s"
  perf script -i "$AB/perf/cs-dwarf.data" 2>/dev/null | python3 "$AB/cs_dwarf.py"
fi
