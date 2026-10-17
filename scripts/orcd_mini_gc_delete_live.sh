#!/bin/bash
# Run ON node9900 (detached). Usage: TINY=200000 OBSERVE=900 orcd_mini_gc_delete_live.sh
# Live delete/GC test on orcd-mini from node9900 (no daemon changes).
# create TINY 256 B files by 16 writers, rm -rf by 16 threads, then sample
# for OBSERVE seconds. Output: $OUT/summary.txt; raw logs stay on the node.
set -u
TINY=${TINY:-200000}; OBSERVE=${OBSERVE:-900}
MNT=/data1/efs/orcd-mini-mnt; T=$MNT/qcat-gcdel-$(date +%s); OUT=${OUT:-/data1/efs/orcd-mini-client/prof/qcat}
mkdir -p "$OUT"; S=$OUT/summary.txt; : > $S
cat > $OUT/create.py <<'P'
import os,sys,time,threading
d=sys.argv[1]; n=int(sys.argv[2]); W=16; blk=b"x"*256
def w(t):
    for i in range(t,n,W):
        p=f"{d}/w{t}/d{i//W//1000}"
        if i//W%1000==0: os.makedirs(p,exist_ok=True)
        with open(f"{p}/f{i}","xb") as f: f.write(blk)
a=time.time(); th=[threading.Thread(target=w,args=(t,)) for t in range(W)]
[x.start() for x in th]; [x.join() for x in th]; e=time.time()-a
print("create: %d files in %.1fs = %.0f files/s"%(n,e,n/e))
P
cat > $OUT/delete.py <<'P'
import os,sys,time,threading
d=sys.argv[1]; W=16; lat=[[] for _ in range(W)]
def rm(t):
    for root,dirs,files in os.walk(f"{d}/w{t}",topdown=False):
        for x in files:
            a=time.perf_counter(); os.unlink(os.path.join(root,x)); lat[t].append(time.perf_counter()-a)
        os.rmdir(root)
a=time.time(); th=[threading.Thread(target=rm,args=(t,)) for t in range(W)]
[x.start() for x in th]; [x.join() for x in th]; e=time.time()-a; os.rmdir(d)
v=sorted(sum(lat,[]))
print("delete: %d unlinks in %.1fs = %.0f files/s; unlink p50=%.2f p90=%.2f p99=%.2f max=%.1f ms"%(len(v),e,len(v)/e,
  1e3*v[len(v)//2],1e3*v[int(len(v)*.9)],1e3*v[int(len(v)*.99)],1e3*v[-1]))
P
echo "start $(date -u +%FT%TZ) tiny=$TINY target=$T" >> $S
python3 $OUT/create.py $T $TINY >> $S 2>&1
echo "create_done $(date -u +%FT%TZ)" >> $S
sleep 30
echo "delete_start $(date -u +%FT%TZ)" >> $S
python3 $OUT/delete.py $T >> $S 2>&1
echo "delete_done $(date -u +%FT%TZ)" >> $S
sleep $OBSERVE
echo "observe_done $(date -u +%FT%TZ)" >> $S
