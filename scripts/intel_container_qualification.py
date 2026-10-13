#!/usr/bin/env python3
"""Run bounded container-only Intel gates on a previously initialized private cluster.

Build/deploy with intel_container_node.py first. No installed service is stopped.
Outputs stay below the supplied local artifact directory, outside Git.
"""
import argparse,concurrent.futures,json,shlex,subprocess,time,uuid
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('gate',choices=['core','peer','mmap','concurrent','lifecycle-create','lifecycle-read','lifecycle-churn','lifecycle-verify','lifecycle-cold','lifecycle-delete','tiny-create','tiny-cold','directory','restart','reclaim']);p.add_argument('--run-id',default=uuid.uuid4().hex[:12],help='unique evidence and namespace suffix');p.add_argument('--base',default='/data1/efs/bench/container-3173be61-20261012');p.add_argument('--output',type=Path,required=True);p.add_argument('--inbox-fold',type=int,choices=[0,1],default=0);p.add_argument('--client-max-threads',type=int,choices=[32,64],default=32);p.add_argument('--deepsteps',action='store_true',help='diagnostic phase/deep-call observer; concurrent gate only, not release acceptance');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
assert a.run_id and all(c.isalnum() or c in '-_' for c in a.run_id),'invalid run id'
s=a.base+'/src';remoteout=a.base+'/artifacts';rows=[]
if a.deepsteps and a.gate!='concurrent':p.error('--deepsteps requires concurrent')
if a.deepsteps:
 # Observer setup precedes all workers; ordinary qualification is untouched.
 observer=Path(__file__).with_name('container_deepsteps_sitecustomize.py')
 for host in ('intel1_efs','intel2_efs'):
  subprocess.run(['ssh',host,shlex.join(['mkdir','-p',remoteout+'/deepsteps'])],check=True)
  subprocess.run(['scp',str(observer),host+':'+remoteout+'/deepsteps/sitecustomize.py'],check=True)
deepsteps_wrapper="import sys,os,socket,threading,runpy\noutput=sys.argv[1];observer=sys.argv[2];suite=sys.argv[3];args=sys.argv[3:]\naddress='/tmp/efs-container-phase-'+str(os.getpid())+'.sock'\nsock=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);sock.bind(address);sock.settimeout(.1)\nstop=threading.Event()\ndef receive():\n with open(output,'wb') as f:\n  while not stop.is_set():\n   try:f.write(sock.recv(4096));f.flush()\n   except socket.timeout:continue\nworker=threading.Thread(target=receive,daemon=True);worker.start()\nos.environ['EFS_POSIX_PHASE_SOCKET']=address;os.environ['EFS_POSIX_PHASE_CLIENT']=os.path.basename(output)\nsys.argv=args\nos.environ['PYTHONPATH']=observer+os.pathsep+os.environ.get('PYTHONPATH','')\ntry:runpy.run_path(suite,run_name='__main__')\nfinally:stop.set();worker.join(1);sock.close();os.unlink(address)\n"
def run(host,index,name,argv,timeout):
 if a.deepsteps:argv=['python3','-c',deepsteps_wrapper,remoteout+'/'+name+'-phases.log',remoteout+'/deepsteps',*argv[1:]]
 c={'name':name,'argv':argv,'timeout':timeout};cmd=['python3',a.base+'/intel_container_node.py','run','--base',a.base,'--host-index',str(index),'--command',json.dumps(c)]
 start=time.monotonic()
 with (a.output/(name+'.log')).open('w') as f:
  try:r=subprocess.run(['ssh',host,shlex.join(cmd)],stdout=f,stderr=subprocess.STDOUT,timeout=timeout+30);rc=r.returncode
  except subprocess.TimeoutExpired:rc=124
 row={'name':name,'host':host,'argv':argv,'returncode':rc,'seconds':round(time.monotonic()-start,3),'status':'PASS' if rc==0 else 'FAIL'};print(json.dumps(row),flush=True);return row
if a.gate=='reclaim':
 def inventory(pair):
  index,host=pair
  cmd=['python3',a.base+'/intel_container_node.py','inventory','--base',a.base,'--host-index',str(index)]
  check=['python3','-c',"import json,sys;from pathlib import Path;assert any(l.split()[4]==sys.argv[1] for l in Path('/proc/self/mountinfo').read_text().splitlines()),'private export is not mounted';print(json.dumps([p.name for p in Path(sys.argv[1]).iterdir()]))",a.base+'/mnt0']
  names=json.loads(subprocess.check_output(['ssh',host,shlex.join(check)],text=True,timeout=15))
  if names:raise ValueError((host,'reclamation requires an empty private export',names))
  return {'host':host,'nodes':json.loads(subprocess.check_output(['ssh',host,shlex.join(cmd)],text=True,timeout=15))}
 begin=time.monotonic();deadline=begin+180;history=[];passed=False
 while time.monotonic()<deadline:
  try:
   with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:snapshot=list(pool.map(inventory,enumerate(['intel1_efs','intel2_efs'])))
  except (ValueError,subprocess.SubprocessError) as exc:
   print(json.dumps({'gate':'reclaim','status':'NOTRUN','reason':str(exc)}),flush=True)
   raise SystemExit(2)
  elapsed=time.monotonic()-begin;history.append({'seconds':round(elapsed,3),'hosts':snapshot})
  passed=elapsed<=180 and all(n['inventory']['data_records']==0 and n['inventory']['container_allocated']==0 for h in snapshot for n in h['nodes'])
  (a.output/('reclaim-'+a.run_id+'-history.json')).write_text(json.dumps(history,indent=2))
  if passed:break
  time.sleep(min(1,max(0,deadline-time.monotonic())))
 print(json.dumps({'gate':'reclaim','status':'PASS' if passed else 'FAIL','seconds':round(time.monotonic()-begin,3),'last':history[-1] if history else None}),flush=True)
 raise SystemExit(0 if passed else 1)
if a.gate=='restart':
 # A cluster-wide phase barrier preserves quorum until EVERY client drains.
 for action,timeout in [('stop-clients',400),('stop-servers',60),('start',90),('mount',90)]:
  jobs=[]
  for index,host in enumerate(['intel1_efs','intel2_efs']):
   argv=['python3',a.base+'/intel_container_node.py',action,'--base',a.base,'--host-index',str(index)]
   if a.inbox_fold:argv+=['--inbox-fold','1']
   if action=='mount':argv+=['--clients',str(4 if index==0 else 3),'--client-max-threads',str(a.client_max_threads)]
   jobs.append((host,index,'restart-'+action+'-intel'+str(index+1)+'-'+a.run_id,argv,timeout))
  with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:phase=list(pool.map(lambda j:run(*j),jobs))
  rows+=phase
  (a.output/'restart-summary.json').write_text(json.dumps(rows,indent=2))
  if any(r['returncode'] for r in phase):raise SystemExit(1)
 raise SystemExit(0)
jobs=[]
for index,host in enumerate(['intel1_efs','intel2_efs']):
 m=a.base+'/mnt0';name=a.gate+'-intel'+str(index+1)+'-'+a.run_id
 if a.gate in ('core','mmap','concurrent','directory'):
  for client in range((4 if index==0 else 3) if a.gate=='concurrent' else 1):
   tag=name+'-'+str(client);argv=['python3',s+'/tests/posix/posix_suite.py',a.base+'/mnt'+str(client),'--suite','extras' if a.gate=='mmap' else 'core','--jobs','1' if a.gate=='directory' else '16','--timeout-s','30','--tag',tag,'--results',remoteout+'/'+tag+'.tsv']
   if a.gate=='mmap':argv+=['--filter','mmap']
   if a.gate=='directory':argv+=['--filter','dir_deep_nesting']
   jobs.append((host,index,tag,argv,600))
 elif a.gate=='peer':jobs.append((host,index,name,['python3',s+'/tests/posix/posix_2client.py','--local',m,a.base+'/mnt1','--parent',name,'--results',remoteout+'/'+name+'.tsv'],600))
 elif a.gate.startswith('tiny-'):
  directory=m+'/tiny-intel'+str(index+1)
  code="import sys,json;from pathlib import Path;sys.path.insert(0,sys.argv[1]);import tiny_churn;p=Path(sys.argv[2]);"
  code+="p.mkdir();print(json.dumps(tiny_churn.exercise(p)))" if a.gate=='tiny-create' else "tiny_churn.verify_and_delete(p);p.rmdir();print('tiny cold hashes and delete PASS')"
  jobs.append((host,index,name,['python3','-c',code,s+'/tests/live',directory],180))
 elif index==0:
  phase={'lifecycle-create':'create','lifecycle-read':'read','lifecycle-churn':'churn','lifecycle-verify':'verify','lifecycle-cold':'verify','lifecycle-delete':'delete'}[a.gate];jobs.append((host,index,name,['python3',s+'/tests/live/storage_engine_ab.py',phase,'--mount',m,'--state',remoteout+'/lifecycle-state.json','--output',remoteout+'/'+name+'-workload.json','--seconds','30' if phase in ('read','churn') else '0'],600))
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:rows=list(pool.map(lambda j:run(*j),jobs))
(a.output/(a.gate+'-summary.json')).write_text(json.dumps(rows,indent=2));raise SystemExit(1 if any(r['returncode'] for r in rows) else 0)
