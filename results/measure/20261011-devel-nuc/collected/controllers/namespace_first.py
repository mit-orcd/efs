import hashlib,json,os,signal,socket,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');src=base/'src';out=base/'namespace-first-unlink';out.mkdir(exist_ok=False)
roots=[Path('/data1/efs/gc-direct-uaiqsu45/n1'),Path('/data1/efs/gc-direct-uaiqsu45/n2'),Path('/home/efs/gc-direct-5i0quq2c/n3'),Path('/home/efs/gc-direct-5i0quq2c/n4')]
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_MD_RAFT_N='4',EFS_RAFT_FLOW='1',EFS_RAFT_DBG='1',EFS_DIROP_DBG='1',EFS_OPID_DBG='1',EFS_OPID_TRACE='1')
procs=[];logs=[];fuse=None;stopped=False

def wait(fn,seconds,label):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if fn():return
  time.sleep(.3)
 raise RuntimeError(label)
def listen(port):
 try:
  with socket.create_connection(('127.0.0.1',port),.2):return True
 except OSError:return False
def snapshots(tag):
 rows=[]
 for i in range(4):
  for cmd in (['raft-status',f'127.0.0.1:{26590+i}'],['raft-lookup',f'127.0.0.1:{26590+i}','1','sentinel']):
   p=subprocess.run([str(src/'efs-mgmt'),*cmd],env=env,capture_output=True,text=True,timeout=20)
   rows.append(dict(node=i+1,command=cmd,rc=p.returncode,stdout=p.stdout,stderr=p.stderr))
 (out/(tag+'.json')).write_text(json.dumps(rows,indent=2))
try:
 wait(lambda:(base/'namespace-probe/restore-exit.txt').exists(),600,'previous probe restoration')
 for i,root in enumerate(roots):
  subprocess.run(['cp','-a','--reflink=auto',root,out/f'n{i+1}'],check=True)
 old=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(old)==4;stopped=True
 for mount in ('mnt','mnt2'):
  subprocess.run(['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+mount],check=True,timeout=180)
 for pid in old:
  fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 wait(lambda:all(not Path('/proc',p).exists() for p in old),180,'normal daemon drain')
 for i in range(4):
  log=(out/f'n{i+1}.log').open('w');logs.append(log)
  cmd=[str(src/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(26590+i),'--storage',str(out/f'n{i+1}'),'--quota','1G','--direct-io']
  if i:cmd+=['--join','127.0.0.1:26590']
  procs.append(subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT));wait(lambda:listen(26590+i),30,'private node')
 time.sleep(5);snapshots('before-mount')
 log=(out/'fuse.log').open('w');logs.append(log)
 fuse=subprocess.Popen([str(src/'efs-fuse'),'127.0.0.1:26590','default','/data1/efs/mnt','-f'],env=env,stdout=log,stderr=subprocess.STDOUT)
 wait(lambda:os.path.ismount('/data1/efs/mnt'),40,'mount')
 path=Path('/data1/efs/mnt/sentinel');steps=[]
 for action in ('stat','unlink','read','stat-after'):
  try:
   if action.startswith('stat'):value=dict(size=path.stat().st_size,ino=path.stat().st_ino)
   elif action=='read':value=dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest())
   else:value=os.unlink(path)
   steps.append(dict(action=action,result=value))
  except OSError as e:steps.append(dict(action=action,errno=e.errno,error=str(e)))
 (out/'actions.json').write_text(json.dumps(steps,indent=2));snapshots('after-unlink')
 print(json.dumps(steps),flush=True)
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
 if fuse and fuse.poll() is None:
  subprocess.run([str(src/'scripts/client.sh'),'stop','/data1/efs/mnt'],check=True,timeout=180);fuse.wait(timeout=20)
 for p in procs:
  if p.poll() is None:p.send_signal(signal.SIGTERM)
 for p in procs:p.wait(timeout=120)
 for log in logs:log.close()
 if stopped:
  normal=dict(env,EFS_RAFT_DBG='0',EFS_DIROP_DBG='0',EFS_OPID_DBG='0',EFS_OPID_TRACE='0',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
  with (out/'restore.log').open('w') as log:subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=300)
  assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
  (out/'restore-exit.txt').write_text('0\n')
