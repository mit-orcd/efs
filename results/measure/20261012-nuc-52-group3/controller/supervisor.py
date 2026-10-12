import os,sys,json,signal,socket,hashlib,subprocess,time,traceback
from pathlib import Path
import matrix
b=matrix.home;o=matrix.out;installed=Path('/data1/efs/src');stopped=False

def wait(fn,seconds,label):
 end=time.monotonic()+seconds
 while not fn():
  assert time.monotonic()<end,label;time.sleep(.5)
def listening(p):
 try:
  with socket.create_connection(('127.0.0.1',p),.3):return True
 except OSError:return False

def run(name,cmd,env=None,timeout=180):
 with (o/(name+'.log')).open('w') as f:rc=subprocess.run(list(map(str,cmd)),env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
 (o/(name+'-exit.txt')).write_text(str(rc));assert rc==0,(name,rc)

def interruption(signum,frame):
 if matrix.active and matrix.active.poll() is None:
  matrix.active.terminate();matrix.active.wait(timeout=30)
 raise KeyboardInterrupt('controlled stop '+str(signum))
signal.signal(signal.SIGTERM,interruption);signal.signal(signal.SIGINT,interruption)
try:
 assert (o/'build-exit.txt').read_text()=='0'
 inputs=json.loads((b/'inputs.json').read_text())['files']
 for variant in ('normal',):
  tree=b/('source-'+variant)
  for n,h in inputs.items():assert hashlib.sha256((tree/n).read_bytes()).hexdigest()==h,n
  for n,r in json.loads((o/(variant+'-images.json')).read_text()).items():assert hashlib.sha256((tree/n).read_bytes()).hexdigest()==r['sha256'],n
 run('before-proof',['python3',b/'proof.py',o/'before-service.json'],timeout=120)
 assert all(listening(p) for p in range(17432,17436))
 procs=subprocess.check_output(['ps','-eo','pid,args'],text=True)
 assert not any('tiny_files.py' in l or 'balanced-load.py' in l or 'drive-load.py' in l for l in procs.splitlines()),'unrelated writer active'
 snapshot=json.loads((o/'before-service.json').read_text());pids=[r['pid'] for r in snapshot['processes'] if r['kind']=='efsd']
 stopped=True
 for m in ('mnt','mnt2'):run('stop-'+m,[installed/'scripts/client.sh','stop','/data1/efs/'+m],env=matrix.base)
 for pid in pids:
  fd=os.pidfd_open(pid);signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 wait(lambda:all(not Path('/proc',str(p)).exists() for p in pids),180,'original daemon drain')
 (o/'cluster-scripts').mkdir(exist_ok=True)
 matrix.matrix()
 for variant in ('normal',):
  tree=b/('source-'+variant)
  for n,h in inputs.items():assert hashlib.sha256((tree/n).read_bytes()).hexdigest()==h,n
 (o/'matrix-exit.txt').write_text('0')
except BaseException:
 (o/'matrix-failure.txt').write_text(traceback.format_exc());(o/'matrix-exit.txt').write_text('1');traceback.print_exc()
finally:
 if stopped:
  try:
   assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')),'retained private mount; refuse unsafe restore'
   assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1,'private daemon remains; preserve stores'
   env=dict(matrix.base,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1',EFS_RAFT_FLOW='1',EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
   run('restore',['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,timeout=360)
   wait(lambda:all(listening(p) for p in range(17432,17436)),180,'original ports')
   run('after-proof',['python3',b/'proof.py',o/'after-service.json'],timeout=120)
   (o/'restoration-exit.txt').write_text('0')
  except BaseException:
   (o/'restoration-failure.txt').write_text(traceback.format_exc());(o/'restoration-exit.txt').write_text('1');traceback.print_exc()
