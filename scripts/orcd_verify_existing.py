#!/usr/bin/env python3
"""Read-only full verification of successful campaign writes, using deterministic payloads.
No failed write is silently counted as acknowledged data; its path is audited separately.
"""
import concurrent.futures,hashlib,json,os,pathlib,socket,sys,time,threading
run=pathlib.Path(sys.argv[1]);out=pathlib.Path(sys.argv[2]);host=socket.gethostname().split('.')[0]
assert host in ['fcstor003','fcstor004','fcstor005','fcstor006']
assert os.path.ismount('/tmp/efs-mount')
original=json.loads((run/(host+'-scale.json')).read_text());out.mkdir(parents=True,exist_ok=True)
counts=original['per_worker'];progress=[0]*len(counts);badcounts=[0]*len(counts);details=[];lock=threading.Lock();start=time.monotonic()
def save(complete=False):
 p=out/(host+'.json');tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(dict(host=host,expected=sum(counts),checked=sum(progress),bad=sum(badcounts),per_worker=progress,errors=details,seconds=time.monotonic()-start,complete=complete),indent=2));tmp.replace(p)
def work(w,n):
 for i in range(n):
  path=pathlib.Path('/tmp/efs-mount')/('scale-'+host)/('w%02d'%w)/('d%05d'%(i//1000))/('f%04d'%(i%1000))
  want=hashlib.sha256(('%s:%d:%d'%(host,w,i)).encode()).digest()*8
  try:
   with path.open('rb') as f:got=f.read(257)
   if got!=want:raise ValueError('payload mismatch: length=%d expected_sha256=%s got_sha256=%s'%(len(got),hashlib.sha256(want).hexdigest(),hashlib.sha256(got).hexdigest()))
  except Exception as e:
   badcounts[w]+=1
   with lock:
    if len(details)<1000:details.append(dict(path=str(path),error=repr(e)))
  progress[w]=i+1
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
 futures=[pool.submit(work,w,n) for w,n in enumerate(counts)]
 while not all(f.done() for f in futures):save();time.sleep(5)
 for f in futures:f.result()
save(True)
uncertain=[]
for e in original['errors']:
 w,i=e['worker'],e['index'];p=pathlib.Path('/tmp/efs-mount')/('scale-'+host)/('w%02d'%w)/('d%05d'%(i//1000))/('f%04d'%(i%1000))
 try:
  with p.open('rb') as f:b=f.read(257)
  expected=hashlib.sha256(('%s:%d:%d'%(host,w,i)).encode()).digest()*8
  uncertain.append(dict(path=str(p),state='matches' if b==expected else 'partial-or-mismatch',size=len(b)))
 except Exception as x:uncertain.append(dict(path=str(p),error=repr(x)))
(out/(host+'-failed-write-outcomes.json')).write_text(json.dumps(uncertain,indent=2))
print(json.dumps(dict(expected=sum(counts),checked=sum(progress),bad=sum(badcounts),seconds=time.monotonic()-start)),flush=True)
sys.exit(bool(sum(badcounts)))
