#!/usr/bin/env python3
"""Mounted read-vote A/B workload. Raw JSON stays in caller-owned output directory."""
import multiprocessing,argparse,concurrent.futures,json,os,socket,subprocess,threading,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('mount');p.add_argument('output');p.add_argument('--seconds',type=int,default=90);p.add_argument('--start',type=float,default=0);a=p.parse_args()
root=Path(a.mount)/('qvote-'+socket.gethostname().split('.')[0]);root.mkdir(exist_ok=True)
fixed=root/'fixed';fixed.mkdir(exist_ok=True)
for i in range(32):(fixed/('f%03d'%i)).write_bytes(bytes([i])*512)
# Establish a correct indexed immutable fixture before timed load, with the existing 30 s freshness budget.
end=time.monotonic()+30
while True:
 r=subprocess.run(['python3',str(fixed/'.efs/ls'),'--json'],capture_output=True,timeout=30)
 try:
  d=json.loads(r.stdout);assert r.returncode==0 and len(d['entries'])==32
  break
 except Exception:
  if time.monotonic()>end:raise RuntimeError((r.returncode,r.stdout[:300],r.stderr[:300]))
  time.sleep(.2)
while time.time()<a.start:time.sleep(.05)
t0=time.monotonic();until=t0+a.seconds;rows=[];lock=threading.Lock()
def record(kind,dt,error=None):
 with lock:rows.append(dict(op=kind,ms=dt*1000,error=error))
def writer(w):
 i=0
 while time.monotonic()<until:
  f=root/('w%d-%08d'%(w,i));data=('%d:%d'%(w,i)).encode().ljust(512,b'x')
  for kind,fn in [('write_fsync',lambda:write(f,data)),('read_verify',lambda:verify(f,data)),('unlink',lambda:f.unlink())]:
   st=time.monotonic();err=None
   try:fn()
   except Exception as e:err=repr(e)
   record(kind,time.monotonic()-st,err)
  i+=1

def write(f,data):
 with f.open('wb') as stream:stream.write(data);stream.flush();os.fsync(stream.fileno())
def verify(f,data):
 actual=f.read_bytes()
 assert actual==data,('payload mismatch',str(f),'expected_length',len(data),'actual_length',len(actual),'expected_prefix',data[:32].hex(),'actual_prefix',actual[:32].hex())
def query(w):
 commands=['ls','find','stats'];i=0
 while time.monotonic()<until:
  kind=commands[i%3];i+=1;st=time.monotonic();err=None
  try:
   cmd=['python3',str(fixed/'.efs'/kind)]+([''] if kind=='find' else [])+['--json']
   r=subprocess.run(cmd,capture_output=True,timeout=30)
   assert r.returncode==0,(r.returncode,r.stderr.decode(errors='replace')[:500])
   d=json.loads(r.stdout)
   if kind in ('ls','find'):assert len(d['entries'])==32,('wrong count',len(d['entries']))
  except Exception as e:err=repr(e)
  record(kind,time.monotonic()-st,err)
# Fork query workers before starting any mounted-I/O threads. FUSE direct-I/O
# on kernels without 738adade96b2 can return stale application-buffer bytes
# when another thread forks while a read is in flight (W121). Query subprocesses
# must run in a separate address space; this does NOT qualify that kernel bug.
def query_process(w):
 query(w)
 Path(a.output+'.query%d.json'%w).write_text(json.dumps(rows))
ctx=multiprocessing.get_context('fork')
queries=[ctx.Process(target=query_process,args=(i,)) for i in range(2)]
for process in queries:process.start()
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
 fs=[ex.submit(writer,i) for i in range(8)]
 for f in fs:f.result()
for i,process in enumerate(queries):
 process.join(timeout=65)
 if process.is_alive():
  process.terminate();process.join();raise RuntimeError('query process timed out')
 if process.exitcode:raise RuntimeError('query process failed: %d'%process.exitcode)
 rows.extend(json.loads(Path(a.output+'.query%d.json'%i).read_text()))
summary={}
for op in sorted({r['op'] for r in rows}):
 r=[x for x in rows if x['op']==op];v=sorted(x['ms'] for x in r)
 summary[op]=dict(count=len(v),errors=sum(x['error'] is not None for x in r),p50=v[int(.5*(len(v)-1))],p95=v[int(.95*(len(v)-1))],p99=v[int(.99*(len(v)-1))],max=max(v))
result=dict(elapsed=time.monotonic()-t0,host=socket.gethostname(),summary=summary,rows=rows)
Path(a.output).write_text(json.dumps(result));print(json.dumps(summary),flush=True)
raise SystemExit(any(x['errors'] for x in summary.values()))
