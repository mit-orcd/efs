import os,sys,time,json,hashlib,subprocess,socket,tempfile,concurrent.futures,traceback,threading
from pathlib import Path
SHA='ecdc8c6034b610a2bf69e7a79664d8a5e990de84'
b=Path('/home/efs/nuc-ecdc-focus');s=b/'source-normal';out=b/'results';work=Path('/data1/efs/nuc-ecdc-focus');work.mkdir(exist_ok=False)
env=dict(os.environ,EFS_MD_RAFT_N='4',EFS_TRANSPORT='tcp',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_PACKED_WRITES='1',EFS_STORAGE_ENGINE='files',EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_REPAIR_EXPERIMENT='0',EFS_REPAIR_INIT_STORE='0')
manifest=json.loads((b/'inputs.json').read_text());assert manifest['commit']==SHA
for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
images=json.loads((out/'normal-images.json').read_text())
for n in ('efsd','efs-fuse','efs-mgmt'):assert hashlib.sha256((s/n).read_bytes()).hexdigest()==images[n]['sha256']
assert not any(os.path.ismount('/data1/efs/'+m)for m in('mnt','mnt2'))
assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
servers=[];clients=[];logs=[];mounts=[Path('/data1/efs/mnt'),Path('/data1/efs/mnt2')];commands=[];verdict={};cleanup=[];command_mu=threading.Lock()
def wait(fn,label,seconds):
 end=time.monotonic()+seconds
 while not fn():
  assert time.monotonic()<end,label;time.sleep(.2)
def run(cmd,label,timeout=60):
 row=dict(label=label,command=list(map(str,cmd)),source=SHA,environment={k:v for k,v in env.items()if k.startswith('EFS_')},timeout=timeout)
 with command_mu:
  commands.append(row);(out/'focused-commands.json').write_text(json.dumps(commands,indent=2))
 start=time.monotonic()
 with(out/(label+'.log')).open('w')as f:rc=subprocess.run(row['command'],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
 with command_mu:
  row.update(returncode=rc,seconds=time.monotonic()-start);(out/'focused-commands.json').write_text(json.dumps(commands,indent=2))
 return rc
try:
 for i in range(4):
  root=work/('n'+str(i+1));root.mkdir();log=(out/('focus-n'+str(i+1)+'.log')).open('w');logs.append(log)
  cmd=[s/'efsd','--node-id',str(i+1),'--addr','127.0.0.1','--port',str(24410+i),'--storage',root,'--quota','4G','--storage-engine','files','--direct-io']
  if i:cmd+=['--join','127.0.0.1:24410']
  proc=subprocess.Popen(list(map(str,cmd)),env=env,stdout=log,stderr=log);servers.append(proc)
  def listening():
   assert proc.poll()is None,'daemon exit'
   try:
    with socket.create_connection(('127.0.0.1',24410+i),.2):return True
   except OSError:return False
  wait(listening,'listen',30)
 time.sleep(3);assert run([s/'efs-mgmt','raft-mkfs','127.0.0.1:24410'],'mkfs')==0
 for i in range(7):
  if i>=2:mounts.append(Path(tempfile.mkdtemp(prefix='efs-focus-'+str(i)+'-',dir='/tmp')))
  m=mounts[i];assert m.is_dir()and not any(m.iterdir());log=(out/('focus-client-'+str(i)+'.log')).open('w');logs.append(log)
  proc=subprocess.Popen([str(s/'efs-fuse'),'127.0.0.1:24410','default',str(m),'-f'],env=env,stdout=log,stderr=log);clients.append(proc)
  wait(lambda:os.path.ismount(m),'mount',40)
 configuration=[]
 for kind,procs in [('efsd',servers),('efs-fuse',clients)]:
  for proc in procs:
   d=Path('/proc',str(proc.pid));actual={v.split('=',1)[0]:v.split('=',1)[1]for v in(d/'environ').read_bytes().decode().split('\0')if'='in v}
   for k,v in env.items():
    if k.startswith('EFS_'):assert actual[k]==v,(k,actual.get(k))
   assert hashlib.sha256((d/'exe').read_bytes()).hexdigest()==images[kind]['sha256']
   configuration.append(dict(kind=kind,pid=proc.pid,exe=os.readlink(d/'exe'),sha256=images[kind]['sha256'],environment={k:v for k,v in actual.items()if k.startswith('EFS_')},cmdline=(d/'cmdline').read_bytes().decode().split('\0')))
 (out/'focused-live-configuration.json').write_text(json.dumps(configuration,indent=2))
 assert '.efs'not in os.listdir(mounts[0]);assert not(mounts[0]/'.efs').exists();verdict['disabled_command_visibility']='PASS'
 def deep(i):
  return run([sys.executable,s/'tests/posix/posix_suite.py',mounts[i],'--suite','core','--filter','dir_deep_nesting','--jobs','16','--timeout-s','30','--tag','focus-'+str(i),'--results',out/('deep-client-'+str(i)+'.tsv')],'deep-client-'+str(i),600)
 start=time.monotonic()
 with concurrent.futures.ThreadPoolExecutor(max_workers=7)as pool:codes=list(pool.map(deep,range(7)))
 verdict['deep_parallel']=dict(returncodes=codes,seconds=time.monotonic()-start,clients=7,selected='dir_deep_nesting and dir_deep_nesting_beyond_64',jobs_per_client=16,timeout_s=30)
 print('focused seven-client deep result '+str(codes),flush=True)
 verdict['basic_write_read_exit']=run([sys.executable,s/'tests/posix/posix_suite.py',mounts[0],'--suite','core','--filter','basic_write_read','--timeout-s','30','--results',out/'basic.tsv'],'basic',120)
 # Small cross-mount acknowledged bytes, metadata and unlink visibility check.
 p=mounts[0]/'focus-light';q=mounts[1]/'focus-light';body=b'focused-payload-'+bytes(range(256))
 with p.open('wb')as f:f.write(body);f.flush();os.fsync(f.fileno())
 assert q.read_bytes()==body;os.rename(p,mounts[0]/'focus-renamed');assert(mounts[1]/'focus-renamed').read_bytes()==body
 (mounts[0]/'focus-renamed').unlink();assert not(mounts[1]/'focus-renamed').exists();verdict['light_peer_mutation']='PASS'
 for n,h in manifest['files'].items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
except BaseException:
 verdict['failure']=traceback.format_exc();traceback.print_exc()
finally:
 for i,proc in enumerate(clients):
  m=mounts[i]
  try:
   rc=run([s/'scripts/client.sh','stop',m],'stop-client-'+str(i),60);assert rc==0;proc.wait(timeout=15)
  except BaseException:
   cleanup.append(traceback.format_exc());subprocess.run(['fusermount3','-uz',str(m)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   proc.terminate()
   try:proc.wait(timeout=5)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
 for proc in servers:
  if proc.poll()is None:proc.terminate()
 for proc in servers:
  try:proc.wait(timeout=20)
  except subprocess.TimeoutExpired:cleanup.append('daemon TERM timeout');proc.kill();proc.wait()
 for f in logs:f.close()
 verdict['cleanup_errors']=cleanup;verdict['mounts_after']={str(m):os.path.ismount(m)for m in mounts};verdict['source']=SHA
 (out/'focused-result.json').write_text(json.dumps(verdict,indent=2))
 print(json.dumps(verdict),flush=True)
