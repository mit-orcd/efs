#!/usr/bin/env python3
"""Normally restart preserved NUC stores after the accepted clock 20k gate."""
import hashlib,json,os,signal,socket,subprocess,sys,time,traceback
from pathlib import Path

base=Path('/data1/efs/devel-nuc-20261011');out=base/'clock-cold';out.mkdir(exist_ok=False)
src=Path('/data1/efs/src');deadline=time.monotonic()+14400;stopped=False
env=dict(os.environ,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1',EFS_TRANSPORT='tcp',
 EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',
 EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
def wait(predicate,seconds,label):
 end=time.monotonic()+seconds
 while not predicate():
  assert time.monotonic()<end,label+' timed out'
  time.sleep(.5)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def listening(port):
 try:
  with socket.create_connection(('127.0.0.1',port),.3):return True
 except OSError:return False
def images_mapped():
 for name,image in images.items():assert digest(src/name)==image['sha256']
 rows=[]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:
   exe=os.readlink(proc/'exe')
   if exe not in (str(src/'efsd'),str(src/'efs-fuse')):continue
   kind=Path(exe).name;assert digest(proc/'exe')==images[kind]['sha256']
   rows.append(dict(pid=int(proc.name),kind=kind,sha256=images[kind]['sha256']))
  except (FileNotFoundError,ProcessLookupError,PermissionError):continue
 assert sum(p['kind']=='efsd' for p in rows)==4 and sum(p['kind']=='efs-fuse' for p in rows)==2
 assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
 assert all(listening(p) for p in range(17432,17436))
 return rows
def payloads():
 rows=[]
 for count,tree,report in (
  (2000,'w52-clock-20000-before-2000-20261011','w52-clock-20000/before-2000'),
  (20000,'w52-clock-20000-20261011','w52-clock-20000')):
  expected=json.loads((base/report/'append-burst.json').read_text())
  assert expected['records']==count and expected['final_fsync_returned_zero']
  assert expected['warm_primary_sha256']==expected['warm_peer_sha256']
  for mount in ('mnt','mnt2'):
   path=Path('/data1/efs',mount,tree,'append-burst')
   value=append_burst.verify(path,count);assert value==expected['warm_primary_sha256']
   rows.append(dict(path=str(path),records=count,sha256=value))
 for sample in json.loads((base/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']:
  for mount in ('mnt','mnt2'):
   path=Path('/data1/efs',mount)/sample['path'];assert digest(path)==sample['sha256']
   rows.append(dict(path=str(path),sha256=sample['sha256']))
 return rows
def restore():
 with (out/'start.log').open('a') as log:
  rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=360).returncode
 assert rc==0
 wait(lambda:all(listening(p) for p in range(17432,17436)),180,'cold listeners')
 rows=images_mapped();checked=payloads()
 (out/'after.json').write_text(json.dumps(dict(processes=rows,payloads=checked,limits='Normal process restart and fresh mounts; not a power-loss test.'),indent=2))
 (out/'restore-exit.txt').write_text('0\n')
try:
 phases=('clock-build','clock-live','clock-install','w52-clock-20000','w52-clock-long-profile')
 while not all((base/p/'run-exit.txt').exists() for p in phases):
  assert time.monotonic()<deadline,'accepted append/profile prerequisite wait timed out'
  for phase in phases:
   assert not (base/phase/'failure.txt').exists(),phase+' failed; leave service running'
   assert not (base/phase/'blocked-prerequisite.json').exists(),phase+' was not run'
  time.sleep(1)
 assert all((base/p/'run-exit.txt').read_text().strip()=='0' for p in phases)
 images=json.loads((base/'clock-install/images.json').read_text())
 sys.path.insert(0,str(src/'tests/live'));import append_burst
 before=images_mapped();checked=payloads()
 (out/'before.json').write_text(json.dumps(dict(images=images,processes=before,payloads=checked),indent=2))
 pids=[p['pid'] for p in before if p['kind']=='efsd'];stopped=True
 for mount in ('mnt','mnt2'):
  with (out/(mount+'-stop.log')).open('w') as log:
   rc=subprocess.run([str(src/'scripts/client.sh'),'stop','/data1/efs/'+mount],stdout=log,stderr=subprocess.STDOUT,timeout=180).returncode
  assert rc==0 and not os.path.ismount('/data1/efs/'+mount)
 for pid in pids:
  fd=os.pidfd_open(pid);signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 wait(lambda:all(not Path('/proc',str(p)).exists() for p in pids),180,'daemon drain')
 assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
 restore();stopped=False;(out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
 if stopped:
  # Retain any live mounts/daemons if drain failed; never force-discard writes.
  assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
  assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
  restore()
