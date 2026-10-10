"""Warm fixed payloads, measure full open/pread/close and held data reads."""
import array,json,os,sys,time,statistics
from pathlib import Path
base=Path(__file__).resolve().parent;cell=base/sys.argv[1];cell.mkdir(exist_ok=True);duration=int(sys.argv[2]);expected=b'W93 immutable read probe'
probes=[]
for tag in ('baseline','overlap'):
 placement=json.loads(Path(f'/data1/efs/w93-overlap-hour-20261009/{tag}/placement.json').read_text())
 for group,path in placement['probes'].items():
  ino=os.stat(path).st_ino;assert (0 if ino&1 else 2)==int(group)
  probes.append(dict(origin=tag,group=int(group),path=path,ino=ino))
for p in probes:
 for i in range(32):assert Path(p['path']).read_bytes()==expected
series=[{k:array.array('Q') for k in ('open','read','close','total')} for p in probes]
def one(p,s):
 a=time.monotonic_ns();fd=os.open(p['path'],os.O_RDONLY);b=time.monotonic_ns()
 try:body=os.pread(fd,4096,0);c=time.monotonic_ns();assert body==expected
 finally:os.close(fd);d=time.monotonic_ns()
 for key,value in zip(('open','read','close','total'),(b-a,c-b,d-c,d-a)):s[key].append(value//1000)
start=time.monotonic();(cell/'start.json').write_text(json.dumps(dict(mono_us=int(start*1e6),utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))))
while time.monotonic()-start<duration:
 for i,p in enumerate(probes):one(p,series[i])
 if sum(len(s['total']) for s in series)>=200000:raise RuntimeError('sample cap reached')
def summary(v):
 a=sorted(v);n=len(a)
 return dict(n=n,mean_us=sum(a)/n,p50_us=a[(n-1)//2],p95_us=a[min(n-1,(n*95+99)//100-1)],p99_us=a[min(n-1,(n*99+99)//100-1)],max_us=a[-1])
result=dict(duration_seconds=time.monotonic()-start,probes=[dict(**p,latency={k:summary(v) for k,v in s.items()}) for p,s in zip(probes,series)])
for group in (0,2):
 combined={k:array.array('Q') for k in ('open','read','close','total')}
 for p,s in zip(probes,series):
  if p['group']==group:
   for k in combined:combined[k].extend(s[k])
 result.setdefault('groups',{})[group]={k:summary(v) for k,v in combined.items()}
result['held_reads']=[]
for p in probes:
 fd=os.open(p['path'],os.O_RDONLY);v=array.array('Q');start=time.monotonic()
 try:
  for i in range(1000):
   a=time.monotonic_ns();assert os.pread(fd,4096,0)==expected;v.append((time.monotonic_ns()-a)//1000)
 finally:os.close(fd)
 result['held_reads'].append(dict(**p,latency=summary(v)))
(cell/'reads.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
