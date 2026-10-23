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
p.add_argument('--balance',action='store_true',help='enable and verify preferred metadata leader placement on the owned fixture')
p.add_argument('--four-voters',action='store_true',help='explicitly grow both owned private groups to four voters; never changes installed topology')
p.add_argument('--mode',choices=('direct','buffered'),default='direct',help='owned daemon storage mode; functional default is direct/TCP')
p.add_argument('--outage-seconds',type=int,default=0,help='with recovery, verify stable successful reads and no leader-term churn while each preferred leader is absent')
p.add_argument('--recovery',action='store_true',help='after timing, stop each group leader and cold restart all owned daemons')
a=p.parse_args();assert 10<=a.samples<=1000
# D39/D40: membership is fixed by the configuration; raft-change is gone.
assert not a.four_voters,'--four-voters needs online membership change, removed by D39'
assert 0<=a.outage_seconds<=120 and (not a.outage_seconds or a.recovery)
a.work.mkdir(parents=True,exist_ok=False)
env=dict(os.environ,EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_READ_PROF='1',EFS_RAFT_OBS='1',EFS_DIRECT_IO='1' if a.mode=='direct' else '0',EFS_METADATA_BALANCE='1' if a.balance else '0')
# D39: the configured member list; efs-mgmt declares as management with it.
conf=a.work/'cluster.conf';conf.write_text(''.join(f'{n+1} 127.0.0.1:{a.port+n}\n' for n in range(4)))
env['EFS_CLUSTER_CONF']=str(conf)
procs=[];logs=[];mutation_attempts=[];stopped=set();commands=[];configuration=[]
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
  assert all(proc.poll() is None for n,proc in enumerate(procs) if n not in stopped),'owned daemon died'
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
  args=[str(a.server),'--cluster',str(conf),'--addr','127.0.0.1','--port',str(a.port+node),'--storage',str(root),'--quota','1G','--writers','0','--direct-io' if a.mode=='direct' else '--no-direct-io']
  commands.append(args)
  procs.append(subprocess.Popen(args,env=env,stdout=log,stderr=log))
  wait(lambda:'listening' in (a.work/f'n{node+1}.log').read_text())
 wait(lambda:any(call(n,'raft-mkfs')['exit']==0 for n in range(4)))
 if a.four_voters:
  for group in (0,2):
   row=call(0,'raft-change',group,'0xf')
   configuration.append({'group':group,**row})
   assert row['exit']==0 and 'raft-change rc=0' in row['text'],row
   def configured():
    status=snapshot()
    return status if all(re.search(rf'group {group} hosted=1 .*voters=0xf joint=0',row['text']) for row in status) else None
   wait(configured)

 placement=[]
 if a.balance:
  def balanced():
   status=snapshot()
   expected={0:0,2:1 if a.four_voters else 2}
   for group,rid in expected.items():
    leaders=[node for node,row in enumerate(status) if re.search(rf'group {group} hosted=1 role=LEADER ',row['text'])]
    if leaders!=[rid]:return None
   return status
  placement=wait(balanced,45)
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
 rows={'mode':a.mode,'transport':'tcp','commands':commands,'four_voters':a.four_voters,'balance':a.balance,'balanced_status':placement,'configuration':configuration,'targets':targets,'parents':parents,'creation':creation,'before':snapshot(),'mutation_attempts':mutation_attempts,'cells':{}}
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
 if a.recovery:
  rows['recovery']=[]
  def verify(label):
   samples=[]
   for node in range(4):
    if node in stopped:continue
    for group,file in targets.items():
     def probe():
      result=call(node,'raft-getattr',file)
      return result if ok(result) else None
     row=wait(probe)
     assert ino(row)==file,(label,node,group,row)
     samples.append({'node':node+1,'group':group,**row})
   rows['recovery'].append({'phase':label,'reads':samples})
  def restart(node):
   procs[node]=subprocess.Popen(commands[node],env=env,stdout=logs[node],stderr=logs[node])
   stopped.remove(node)
   def listening():
    try:
     with socket.create_connection(('127.0.0.1',a.port+node),timeout=.2):return True
    except OSError:return False
   wait(listening)
  for group in (0,2):
   def leader():
    for node in range(4):
     if re.search(rf'group {group} hosted=1 role=LEADER ',call(node,'raft-status')['text']):return node+1
    return None
   node=wait(leader)-1
   procs[node].terminate();procs[node].wait(timeout=30);stopped.add(node)
   verify(f'group-{group}-leader-{node+1}-stopped')
   if a.outage_seconds:
    def leaders_now():
     state={}
     for peer in range(4):
      if peer in stopped:continue
      row=call(peer,'raft-status')
      assert row['exit']==0,row
      for g in (0,2):
       match=re.search(rf'group {g} hosted=1 role=LEADER leader=\d+ term=(\d+)',row['text'])
       if match:
        assert g not in state,('multiple leaders during outage',state,row)
        state[g]=(peer+1,int(match.group(1)))
     assert len(state)==2,('missing leader during outage',state)
     return state
    settled=leaders_now();outage=[];until=time.monotonic()+a.outage_seconds
    while time.monotonic()<until:
     state=leaders_now()
     assert state==settled,('leadership churn with unavailable preferred peer',settled,state)
     for peer in range(4):
      if peer in stopped:continue
      for g,file in targets.items():
       row=call(peer,'raft-getattr',file)
       assert ok(row) and ino(row)==file,('outage read failed',peer,g,row)
       outage.append({'node':peer+1,'group':g,**row})
     time.sleep(1)
    rows['recovery'][-1]['outage']={'seconds':a.outage_seconds,'leaders':settled,'reads':outage}
   restart(node)
   verify(f'group-{group}-leader-rejoined')
   if a.balance:rows['recovery'][-1]['balanced_status']=wait(balanced,45)
  for node,proc in enumerate(procs):
   proc.terminate()
  for node,proc in enumerate(procs):
   proc.wait(timeout=30);stopped.add(node)
  for node in range(4):restart(node)
  verify('all-daemons-cold-restarted')
  rows['cold_status']=wait(balanced,45) if a.balance else snapshot()
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
