import os,sys,time,json,subprocess,concurrent.futures
from pathlib import Path
base=Path('/data1/efs/amd-v031-15e2a531-20261012-live');source=Path('/data1/efs/amd-v031-15e2a531-20261012/source');out=base/'evidence';out.mkdir(parents=True);tmp=Path('/tmp/efs-amd-v031-15e2a531');tmp.mkdir(exist_ok=False)
server=[];clients=[];handles=[];env=dict(os.environ,EFS_TRANSPORT='tcp',EFS_PACK_PROF='0',EFS_MD_RAFT_N='4',EFS_PACKED_WRITES='0',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_REPAIR_EXPERIMENT='0',EFS_REPAIR_INIT_STORE='0')
def wait(fn,label,seconds):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if fn():print(label+' PASS',flush=True);return
  time.sleep(.1)
 raise RuntimeError(label+' timeout')
def log(name):
 f=(out/name).open('wb');handles.append(f);return f
def serving(target):return os.path.ismount(target) and subprocess.run(['timeout','2','stat',target],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode==0
try:
 for i in range(4):
  storage=out/('n%d'%(i+1));storage.mkdir();f=log('n%d.log'%(i+1));cmd=[str(source/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(25100+i),'--storage',str(storage),'--quota','4G','--direct-io','--storage-engine','files']
  if i:cmd+=['--join','127.0.0.1:25100']
  server.append(subprocess.Popen(cmd,env=env,stdout=f,stderr=f));time.sleep(.8);assert server[-1].poll() is None
 (out/'runtime-config.json').write_text(json.dumps({'servers':[{'pid':p.pid,'environment':[v for v in Path('/proc/%d/environ'%p.pid).read_bytes().decode().split(chr(0)) if v.startswith('EFS_')]} for p in server]},indent=2));time.sleep(3);subprocess.run([str(source/'efs-mgmt'),'raft-mkfs','127.0.0.1:25100'],env=env,check=True,timeout=30)
 mounts=[]
 for i in range(7):
  target=tmp/('deep-mnt%d'%i);target.mkdir();mounts.append(target);f=log('fuse-%d.log'%i)
  clients.append(subprocess.Popen([str(source/'efs-fuse'),'127.0.0.1:25100','default',str(target),'-f'],env=env,stdout=f,stderr=f));wait(lambda:serving(target),'mount%d'%i,40)
 (out/'client-config.json').write_text(json.dumps([{'pid':p.pid,'environment':[v for v in Path('/proc/%d/environ'%p.pid).read_bytes().decode().split(chr(0)) if v.startswith('EFS_')]} for p in clients],indent=2))
 def run(i):
  trace=out/('syscalls-%d'%i);trace.mkdir();e=dict(env,EFS_DIR_TRACE=str(trace),EFS_DIR_ORIGINAL_SUITE=str(source/'tests/posix/posix_suite.py'),EFS_DIR_OBSERVER_ROOT=str(mounts[i]))
  cmd=['python3',str(source/'tests/posix/posix_suite.py'),str(mounts[i]),'--suite','core','--jobs','16','--timeout-s','30','--tag','deep%d'%i,'--results',str(out/('deep-%d.tsv'%i))]
  start=time.monotonic();f=log('deep-%d.log'%i);p=subprocess.run(cmd,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=120)
  row={'client':i,'command':cmd,'seconds':round(time.monotonic()-start,3),'returncode':p.returncode};print(json.dumps(row),flush=True);return row
 with concurrent.futures.ThreadPoolExecutor(max_workers=7) as pool:rows=list(pool.map(run,range(7)))
 f=log('peer.log');peer_rc=subprocess.run(['python3',str(source/'tests/posix/posix_2client.py'),'--local',str(mounts[0]),str(mounts[1]),'--parent','ordinary-peer','--results',str(out/'peer.tsv')],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=180).returncode
 (out/'peer-result.json').write_text(json.dumps({'returncode':peer_rc}))
 (out/'deep-results.json').write_text(json.dumps(rows,indent=2))
 # Record graceful failures honestly; outer controller owns fixture cleanup.
 stops=[]
 for i,target in enumerate(mounts):
  f=log('stop-%d.log'%i)
  try:rc=subprocess.run([str(source/'scripts/client.sh'),'stop',str(target)],cwd=source,env=env,stdout=f,stderr=f,timeout=60).returncode
  except subprocess.TimeoutExpired:rc=124
  stops.append({'client':i,'returncode':rc})
  if rc:
   stops.extend({'client':j,'returncode':None,'status':'NOTRUN','reason':'earlier graceful stop refused'} for j in range(i+1,len(mounts)));break
 (out/'stop-results.json').write_text(json.dumps(stops,indent=2))
 sys.exit(1 if peer_rc or any(r.get('returncode') for r in rows+stops) else 0)
finally:
 for p in clients+server:
  if p.poll() is None:p.terminate()
 for p in clients+server:
  try:p.wait(timeout=10)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 for f in handles:f.flush();f.close()
