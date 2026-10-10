"""Independent XFS overwrite+fdatasync latency; isolated scratch files only.

No service/quota files are used. QD denotes concurrent independent files, not
an EFS API queue. Warm allocation is excluded. This is not a power-loss proof.
"""
import concurrent.futures,hashlib,json,os,struct,tempfile,time
from pathlib import Path
base=Path(__file__).resolve().parent;results=[]
def summary(v):
 a=sorted(v);n=len(a)
 return dict(n=n,mean_us=sum(a)/n,p50_us=a[(n*50+99)//100-1],p99_us=a[(n*99+99)//100-1],max_us=a[-1])
for parent in ('/data1/efs','/home/efs/additional-work-dir'):
 for qd in (1,4):
  scratch=Path(tempfile.mkdtemp(prefix='w93-raw-sync-',dir=parent));paths=[]
  bodies=[struct.pack('>Q',i)+(hashlib.sha256(str(i).encode()).digest()*128)[:4088] for i in range(144)]
  def worker(w):
   path=scratch/f'file{w}';paths.append(path)
   fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600);write=[];sync=[]
   try:
    for i,body in enumerate(bodies):
     a=time.monotonic_ns();n=os.pwrite(fd,body,0);b=time.monotonic_ns();assert n==4096
     os.fdatasync(fd);c=time.monotonic_ns()
     if i>=16:write.append((b-a)//1000);sync.append((c-b)//1000)
    assert os.pread(fd,4096,0)==bodies[-1]
    return dict(worker=w,write=summary(write),sync=summary(sync))
   finally:os.close(fd)
  try:
   start=time.monotonic()
   with concurrent.futures.ThreadPoolExecutor(max_workers=qd) as pool:rows=list(pool.map(worker,range(qd)))
   elapsed=time.monotonic()-start
   results.append(dict(parent=parent,real_parent=str(Path(parent).resolve()),qd=qd,bytes=4096,warmup_per_worker=16,measured_per_worker=128,wall_s_including_warmup=elapsed,workers=rows))
  finally:
   for p in paths:p.unlink()
   scratch.rmdir()
(base/'raw-sync.json').write_text(json.dumps(dict(utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),results=results,physical_power_loss=False,overwrite_not_append=True),indent=2))
for row in results:print(json.dumps(row))
