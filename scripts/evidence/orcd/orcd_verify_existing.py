#!/usr/bin/env python3
"""Read-only full verification of successful campaign writes, using deterministic payloads.
No failed write is silently counted as acknowledged data; its path is audited separately.
"""
import argparse,concurrent.futures,hashlib,json,os,pathlib,socket,time,threading
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("run");parser.add_argument("out")
parser.add_argument("--resume",help="prior checkpoint directory; continue unchecked records and retain every prior failure")
parser.add_argument("--fixture-root",default="/tmp/efs-mount",help="directory containing scale-HOST; keeps new scale fixtures separate")
args=parser.parse_args()
run=pathlib.Path(args.run);out=pathlib.Path(args.out);host=socket.gethostname().split('.')[0]
assert host in ['fcstor003','fcstor004','fcstor005','fcstor006']
assert os.path.ismount('/tmp/efs-mount')
fixture=pathlib.Path(args.fixture_root)
assert fixture.is_absolute() and '..' not in fixture.parts
assert os.path.commonpath([str(fixture), '/tmp/efs-mount']) == '/tmp/efs-mount', 'fixture must remain inside the campaign mount'
original=json.loads((run/(host+'-scale.json')).read_text());out.mkdir(parents=True,exist_ok=True)
counts=original['per_worker'];progress=[0]*len(counts);badcounts=[0]*len(counts);details=[];prior_bad=0;prior_checked=0
if args.resume:
 previous=pathlib.Path(args.resume)
 assert previous.resolve()!=out.resolve(), 'preserve the original checkpoint directory'
 baseline=json.loads((previous/(host+'.json')).read_text())
 assert baseline['host']==host and baseline['expected']==sum(counts)
 assert baseline.get('fixture_root','/tmp/efs-mount')==args.fixture_root, 'checkpoint belongs to another fixture'
 progress=baseline['per_worker'][:]
 assert len(progress)==len(counts) and all(type(n)==int and 0<=n<=counts[w] for w,n in enumerate(progress))
 assert sum(progress)==baseline['checked']
 prior_bad=baseline['bad'];prior_checked=baseline['checked'];details=baseline['errors'][:]
 assert type(prior_bad)==int and 0<=prior_bad<=prior_checked
lock=threading.Lock();start=time.monotonic()
def save(complete=False):
 p=out/(host+'.json');tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(dict(host=host,fixture_root=args.fixture_root,expected=sum(counts),checked=sum(progress),bad=prior_bad+sum(badcounts),prior_checked=prior_checked,per_worker=progress,errors=details,seconds=time.monotonic()-start,complete=complete),indent=2));tmp.replace(p)
def work(w,n):
 for i in range(progress[w],n):
  path=fixture/('scale-'+host)/('w%02d'%w)/('d%05d'%(i//1000))/('f%04d'%(i%1000))
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
 w,i=e['worker'],e['index'];p=fixture/('scale-'+host)/('w%02d'%w)/('d%05d'%(i//1000))/('f%04d'%(i%1000))
 try:
  with p.open('rb') as f:b=f.read(257)
  expected=hashlib.sha256(('%s:%d:%d'%(host,w,i)).encode()).digest()*8
  uncertain.append(dict(path=str(p),state='matches' if b==expected else 'partial-or-mismatch',size=len(b)))
 except Exception as x:uncertain.append(dict(path=str(p),error=repr(x)))
(out/(host+'-failed-write-outcomes.json')).write_text(json.dumps(uncertain,indent=2))
print(json.dumps(dict(expected=sum(counts),checked=sum(progress),bad=prior_bad+sum(badcounts),prior_checked=prior_checked,seconds=time.monotonic()-start)),flush=True)
raise SystemExit(bool(prior_bad+sum(badcounts)))
