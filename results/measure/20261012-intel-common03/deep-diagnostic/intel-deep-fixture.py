import os,sys,time,json,subprocess,concurrent.futures
from pathlib import Path
base=Path('/data1/efs/intel-common03-deep-20261012');source=base/'normal';out=base/'evidence/live';out.mkdir();tmp=Path(os.environ['TMPDIR']).parent
server=[];clients=[];handles=[];env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_PACKED_WRITES='1',EFS_COMMANDS='1',EFS_COMMANDS_SERVER='1')
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
  storage=out/('n%d'%(i+1));storage.mkdir();f=log('n%d.log'%(i+1));cmd=[str(source/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(34510+i),'--storage',str(storage),'--quota','4G','--direct-io']
  if i:cmd+=['--join','127.0.0.1:34510']
  server.append(subprocess.Popen(cmd,env=env,stdout=f,stderr=f));time.sleep(.8);assert server[-1].poll() is None
 time.sleep(3);subprocess.run([str(source/'efs-mgmt'),'raft-mkfs','127.0.0.1:34510'],env=env,check=True,timeout=30)
 mounts=[]
 for i in range(7):
  target=tmp/('deep-mnt%d'%i);target.mkdir();mounts.append(target);f=log('fuse-%d.log'%i)
  clients.append(subprocess.Popen([str(source/'efs-fuse'),'127.0.0.1:34510','default',str(target),'-f'],env=env,stdout=f,stderr=f));wait(lambda:serving(target),'mount%d'%i,40)
 def run(i):
  trace=out/('syscalls-%d'%i);trace.mkdir();e=dict(env,EFS_DEEP_TRACE_ROOT=str(mounts[i]),EFS_DEEP_TRACE_DIR=str(trace),EFS_POSIX_PHASE_CLIENT=str(i))
  cmd=['python3',str(base/'intel-deep-case.py'),str(source/'tests/posix/posix_suite.py'),str(mounts[i]),'--suite','all','--jobs','16','--timeout-s','30','--filter','dir_deep_nesting','--tag','deep%d'%i,'--results',str(out/('deep-%d.tsv'%i))]
  start=time.monotonic();f=log('deep-%d.log'%i);p=subprocess.run(cmd,env=e,stdout=f,stderr=subprocess.STDOUT,timeout=120)
  row={'client':i,'command':cmd,'seconds':round(time.monotonic()-start,3),'returncode':p.returncode};print(json.dumps(row),flush=True);return row
 with concurrent.futures.ThreadPoolExecutor(max_workers=7) as pool:rows=list(pool.map(run,range(7)))
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
 sys.exit(1 if any(r.get('returncode') for r in rows+stops) else 0)
finally:
 for f in handles:f.flush();f.close()
