"""Bounded create/write/close workload: four workers per verified Raft group.

Preparation scatters directories, selects equal group populations, and is
excluded from the timed interval. At most 1,024,000 files; maximum observed
latency storage is bounded by that cap. No existing path is overwritten.
"""
import argparse,array,hashlib,json,os,signal,threading,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('evidence',type=Path)
a=p.parse_args();a.root.mkdir();a.evidence.mkdir(exist_ok=True)
stop=threading.Event();signal.signal(signal.SIGUSR1,lambda *_:stop.set())
groups={0:[],2:[]};extras=[]
for i in range(2048):
 d=a.root/f'd{i}';d.mkdir();ino=d.stat().st_ino;g=0 if ino&1 else 2
 if len(groups[g])<512:groups[g].append(dict(path=str(d),ino=ino,group=g))
 else:extras.append(str(d))
 if all(len(v)==512 for v in groups.values()):break
else:raise RuntimeError('unable to select balanced groups')
probes={g:str(Path(v[0]['path'])/'read-probe') for g,v in groups.items()}
for path in probes.values():
 with Path(path).open('xb',buffering=0) as f:f.write(b'W93 immutable read probe');os.fsync(f.fileno())
(a.evidence/'placement.json').write_text(json.dumps(dict(groups=groups,extras=extras,probes=probes),indent=2))
(a.evidence/'ready.json').write_text(json.dumps(dict(directories=sum(map(len,groups.values())))))
while not (a.evidence/'GO').exists():
 if stop.wait(.1):raise RuntimeError('stopped before timed workload')
counts=[0]*8;errors=[];rows=[];gate=threading.Barrier(9)
for w in range(8):rows.append({k:array.array('Q') for k in ('open','write','close','total')})
def worker(w):
 group=0 if w<4 else 2;dirs=groups[group][(w%4)*128:(w%4+1)*128]
 gate.wait()
 try:
  for i in range(128000):
   if stop.is_set():break
   d=Path(dirs[i//1000]['path']);body=hashlib.sha256(f'{w}:{i}'.encode()).digest()*16
   size=1+((w*65537+i)*1103515245+12345)%512;body=body[:size]
   t0=time.monotonic_ns();fd=os.open(d/f'f{w}-{i}',os.O_CREAT|os.O_EXCL|os.O_WRONLY,0o600);t1=time.monotonic_ns()
   try:
    if i%1000==0:assert (0 if os.fstat(fd).st_ino&1 else 2)==group,'file placement differed from directory'
    assert os.write(fd,body)==len(body)
   finally:
    t2=time.monotonic_ns();os.close(fd);t3=time.monotonic_ns()
   for key,val in (('open',t1-t0),('write',t2-t1),('close',t3-t2),('total',t3-t0)):rows[w][key].append(val//1000)
   counts[w]+=1
 except BaseException as e:errors.append(dict(worker=w,error=repr(e)));stop.set()
threads=[threading.Thread(target=worker,args=(w,)) for w in range(8)]
for t in threads:t.start()
start=time.monotonic();(a.evidence/'load-start.json').write_text(json.dumps(dict(mono_us=int(start*1e6),utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))));gate.wait()
print('START',flush=True)
with (a.evidence/'progress.jsonl').open('w') as f:
 while any(t.is_alive() for t in threads):
  f.write(json.dumps(dict(seconds=time.monotonic()-start,counts=list(counts),groups={0:sum(counts[:4]),2:sum(counts[4:])}))+'\n');f.flush()
  stop.wait(1) if not stop.is_set() else time.sleep(.1)
for t in threads:t.join()
final=dict(seconds=time.monotonic()-start,counts=counts,errors=errors,workers=[])
def summary(values):
 values=sorted(values);n=len(values)
 return dict(n=n,mean_us=sum(values)/n if n else None,**{f'p{q}_us':values[min(n-1,(n*q+99)//100-1)] if n else None for q in (50,95,99)},max_us=values[-1] if n else None)
for w in range(8):final['workers'].append(dict(worker=w,group=0 if w<4 else 2,latency={k:summary(v) for k,v in rows[w].items()}))
for key in ('open','write','close','total'):
 raw=array.array('Q');[raw.extend(row[key]) for row in rows]
 final.setdefault('latency',{})[key]=summary(raw)
(a.evidence/'load-final.json').write_text(json.dumps(final,indent=2));print(json.dumps(final),flush=True)
assert not errors,errors
