#!/usr/bin/env python3
"""Private four-daemon ReadIndex comparison; no installed stores or mounts.
Times include management CLI startup. Reports all samples; no capacity claim.
"""
import argparse,concurrent.futures,json,math,os,re,socket,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--server',type=Path,required=True)
p.add_argument('--work',type=Path,required=True)
p.add_argument('--samples',type=int,default=100)
p.add_argument('--port',type=int,default=27440)
a=p.parse_args();assert 10<=a.samples<=1000
a.work.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_READ_PROF='1',EFS_RAFT_OBS='1')
procs=[];logs=[];mutation_attempts=[]
for port in range(a.port,a.port+4):
 with socket.socket() as s:
  s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
  s.bind(('127.0.0.1',port))
def call(node,op,*args):
 t=time.monotonic_ns()
 r=subprocess.run([str(a.source/'efs-mgmt'),op,f'127.0.0.1:{a.port+node}',*map(str,args)],env=env,capture_output=True,text=True,timeout=10)
 return {'ms':(time.monotonic_ns()-t)/1e6,'exit':r.returncode,'text':r.stdout+r.stderr}
def wait(fn,seconds=30):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  assert all(p.poll() is None for p in procs),'owned daemon died'
  result=fn()
  if result:return result
  time.sleep(.05)
 raise AssertionError('fixture readiness timeout')
def ok(row):return row['exit']==0 and 'status=0' in row['text']
def mutation(op,*args):
 def attempt():
  for node in range(4):
   row=call(node,op,*args)
   mutation_attempts.append({'node':node+1,'operation':op,'args':list(args),**row})
   if ok(row):return row
  return None
 return wait(attempt)
def ino(row):return int(re.search(r'\bino=(\d+)',row['text']).group(1))
def snapshot():return [call(n,'raft-status') for n in range(4)]
try:
 for node in range(4):
  root=a.work/f'n{node+1}';root.mkdir();log=(a.work/f'n{node+1}.log').open('w');logs.append(log)
  args=[str(a.server),'--node-id',str(node+1),'--addr','127.0.0.1','--port',str(a.port+node),'--storage',str(root),'--quota','1G','--writers','0','--no-direct-io']
  if node:args+=['--join',f'127.0.0.1:{a.port}']
  procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
  wait(lambda:'listening' in (a.work/f'n{node+1}.log').read_text())
 wait(lambda:any(call(n,'raft-mkfs')['exit']==0 for n in range(4)))
 targets={};parents={};creation=[]
 for i in range(64):
  directory=ino(mutation('raft-create',1,f'd-{i}','040755'))
  if i%2:mutation('raft-create',directory,'placement-pad','0100644')
  file=ino(mutation('raft-create',directory,'target','0100644'))
  group=0 if file&1 else 2
  creation.append({'directory':directory,'file':file,'group':group})
  if group not in targets:targets[group]=file;parents[group]=directory
  if len(targets)==2:break
 assert len(targets)==2,('both placement classes required',creation)
 rows={'targets':targets,'parents':parents,'creation':creation,'before':snapshot(),'mutation_attempts':mutation_attempts,'cells':{}}
 def reads(key,node,file):
  samples=[call(node,'raft-getattr',file) for _ in range(a.samples)]
  assert all(ok(r) for r in samples),(key,[r for r in samples if not ok(r)])
  return key,samples
 def writes(group):
  for i in range(a.samples):
   mutation('raft-create',parents[group],f'load-{i}','0100644')
   mutation('raft-unlink',parents[group],f'load-{i}')
 for load in ('idle','writes'):
  with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
   jobs=[pool.submit(reads,f'{load}-g{g}-node{n+1}',n,targets[g]) for g in (0,2) for n in (0,1)]
   writers=[pool.submit(writes,g) for g in (0,2)] if load=='writes' else []
   for job in jobs:
    key,samples=job.result();values=sorted(r['ms'] for r in samples)
    rows['cells'][key]={'samples':samples,'p50_ms':values[math.ceil(len(values)*.5)-1],'p95_ms':values[math.ceil(len(values)*.95)-1],'p99_ms':values[math.ceil(len(values)*.99)-1],'max_ms':max(values)}
   for job in writers:job.result()
 rows['after']=snapshot()
 (a.work/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
 print(json.dumps({k:{m:v[m] for m in ('p50_ms','p95_ms','p99_ms','max_ms')} for k,v in rows['cells'].items()},indent=2),flush=True)
finally:
 for proc in procs:
  if proc.poll() is None:proc.terminate()
 failures=[]
 for proc in procs:
  try:proc.wait(timeout=30)
  except subprocess.TimeoutExpired:failures.append(proc.pid)
 for log in logs:log.close()
 assert not failures,('owned daemons refused graceful exit; no forced kill',failures)
