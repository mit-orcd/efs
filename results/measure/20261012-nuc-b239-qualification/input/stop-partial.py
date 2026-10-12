import os,json,signal,hashlib,subprocess,select,time,importlib.util,traceback,socket
from pathlib import Path
b=Path('/data1/efs/nuc-b239-qualification');src=b/'source';out=b/'partial-stop';out.mkdir();servers=[];handles=[]
def hashfile(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def write(n,v):(out/n).write_text(json.dumps(v,indent=2))
def run(n,c,env=None,timeout=180):
 with (out/(n+'.log')).open('w') as f:rc=subprocess.run(list(map(str,c)),cwd=src,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
 (out/(n+'-exit.txt')).write_text(str(rc));assert rc==0,(n,rc)
def wait(fn,secs,label):
 end=time.monotonic()+secs
 while not fn():
  assert time.monotonic()<end,label
  time.sleep(.25)
def listener(p):
 try:
  with socket.create_connection(('127.0.0.1',p),.3):return True
 except OSError:return False
try:
 hold=json.loads((b/'queued-launch-hold.json').read_text());child=hold['active_fixture']['pid']
 for pid in (197319,child):
  fd=os.pidfd_open(pid);signal.pidfd_send_signal(fd,signal.SIGSTOP);os.close(fd)
 assert Path('/proc/186416/stat').read_text().rsplit(')',1)[1].split()[0]=='T'
 images=json.loads((b/'native/fault-images.json').read_text());owned=[]
 for p in Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   if os.readlink(p/'exe')!=str(src/'efsd'):continue
   args=(p/'cmdline').read_bytes().decode().split('\0');args=[x for x in args if x]
   assert hashfile(p/'exe')==images['efsd']['sha256']
   selected={x.split('=',1)[0]:x.split('=',1)[1] for x in (p/'environ').read_bytes().decode().split('\0') if x.startswith('EFS_') and '=' in x}
   owned.append(dict(pid=int(p.name),args=args,environment=selected))
  except (FileNotFoundError,ProcessLookupError,PermissionError):continue
 assert len(owned)==4
 owned.sort(key=lambda x:int(x['args'][x['args'].index('--node-id')+1]));env=dict(os.environ,**owned[0]['environment'])
 work=Path('/data1/efs/gc-direct-a1kmj9hw');progress=json.loads((work/'append-burst-progress.json').read_text());write('before.json',dict(owned=owned,progress=progress,reason='Manager supersedes old source; producer paused, durable boundary before normal interruption'))
 path=Path('/data1/efs/mnt/append-burst');fd=os.open(path,os.O_RDONLY)
 try:
  began=time.monotonic();os.fsync(fd);elapsed=time.monotonic()-began
 finally:os.close(fd)
 spec=importlib.util.spec_from_file_location('append_burst',src/'tests/live/append_burst.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
 count=path.stat().st_size//4096;assert count>=progress['records'] and count<20000
 digest=m.verify(path,count);assert m.verify(Path('/data1/efs/mnt2/append-burst'),count)==digest
 write('durable-warm.json',dict(records=count,bytes=count*4096,fsync_returned_zero=True,fsync_seconds=elapsed,primary_sha256=digest,peer_sha256=digest,gate='Partial orderly-stop witness only; 20k NOTRUN/STOPPED'))
 for mount in ('mnt','mnt2'):run('quiesce-'+mount,[src/'efs-fuse','--stop','/data1/efs/'+mount],env)
 fd=os.pidfd_open(child);signal.pidfd_send_signal(fd,signal.SIGTERM);signal.pidfd_send_signal(fd,signal.SIGCONT)
 assert select.select([fd],[],[],180)[0],'producer normal shutdown incomplete; do not force'
 fields=Path('/proc',str(child),'stat').read_text().rsplit(')',1)[1].split();rc=os.waitstatus_to_exitcode(int(fields[49]));os.close(fd)
 write('producer-exit.json',dict(pid=child,exit=rc,intentional_stop=True,full20k='STOPPED/NOTRUN'))
 assert not any(os.path.ismount('/data1/efs/'+x) for x in ('mnt','mnt2'))
 assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
 for row in owned:
  h=(out/('cold-n'+row['args'][row['args'].index('--node-id')+1]+'.log')).open('w');handles.append(h)
  p=subprocess.Popen(row['args'],env=dict(os.environ,**row['environment']),stdout=h,stderr=h);servers.append(p)
  port=int(row['args'][row['args'].index('--port')+1]);wait(lambda:listener(port) if p.poll() is None else (_ for _ in ()).throw(RuntimeError('cold daemon exited')),120,'cold listener')
 for i,mount in enumerate(('mnt','mnt2')):run('cold-mount-'+mount,[src/'scripts/client.sh','127.0.0.1:'+str(26590+i),'/data1/efs/'+mount],dict(env,EFS_FUSE_LOG=str(out/(mount+'-cold-fuse.log'))))
 for mount in ('mnt','mnt2'):assert m.verify(Path('/data1/efs')/mount/'append-burst',count)==digest
 write('cold.json',dict(records=count,bytes=count*4096,sha256=digest,both_fresh_mounts=True,full20k='STOPPED/NOTRUN'))
 Path('/data1/efs/mnt/append-burst').unlink()
 roots=[Path(row['args'][row['args'].index('--storage')+1]) for row in owned]
 def drained():
  rows=[]
  for i in range(4):
   text=subprocess.check_output([str(src/'efs-mgmt'),'gc-status','127.0.0.1:'+str(26590+i)],text=True,timeout=15)
   rows.append(text)
  import re
  sample=[{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',l)} for text in rows for l in text.splitlines() if 'sampled=1' in l]
  good={r.get('group') for r in sample}=={0,2} and all(r.get('pending_estimate')==r.get('reap_seen')==r.get('orphan_seen')==0 for r in sample)
  if good:write('gc-drained.json',dict(status=rows))
  return good
 wait(drained,180,'partial workload GC drain')
 wait(lambda:all(not any(p.is_file() for p in (root/'data/exports/1').rglob('*')) for root in roots),180,'partial physical empty')
 write('physical-cleanup.json',dict(roots=list(map(str,roots)),all_payload_files_empty=True))
 for mount in ('mnt','mnt2'):run('cold-stop-'+mount,[src/'scripts/client.sh','stop','/data1/efs/'+mount],env)
 for p in servers:p.terminate()
 for p in servers:assert p.wait(timeout=180)==0
 for h in handles:h.close()
 (out/'run-exit.txt').write_text('0')
 # Original guard now records actual stopped fixture exit and resumes finally restoration only.
 gfd=os.pidfd_open(197319);signal.pidfd_send_signal(gfd,signal.SIGCONT);os.close(gfd)
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1');raise
