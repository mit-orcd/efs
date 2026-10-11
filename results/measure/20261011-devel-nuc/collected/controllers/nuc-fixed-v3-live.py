#!/usr/bin/env python3
"""Bounded NUC integrated-code gates; preserve installed source and stores."""
import hashlib,json,os,signal,socket,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');src=base/'candidate';out=base/'fixed-v3-live';out.mkdir(exist_ok=False)
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_MD_RAFT_N='4',EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
def wait(fn,seconds,label):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if fn():return
  time.sleep(.5)
 raise RuntimeError(label+' timed out')
def run(name,args,seconds=1800):
 print('START',name,time.strftime('%FT%TZ',time.gmtime()),flush=True)
 with (out/(name+'.log')).open('w') as log:
  rc=subprocess.run(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=seconds).returncode
 (out/(name+'-exit.txt')).write_text(str(rc)+'\n');print('END',name,rc,flush=True);return rc
def port(p):
 try:
  with socket.create_connection(('127.0.0.1',p),.3):return True
 except OSError:return False
stopped=False
try:
 wait(lambda:(base/'fixed-v2-live/restore-exit.txt').exists(),900,'previous suite restoration')
 wait(lambda:(base/'fixed-v3-build/native-exit.txt').exists(),2400,'native suite')
 assert (base/'fixed-v3-build/native-exit.txt').read_text().strip()=='0','native failed; do not stop regular service'
 (out/'binaries.json').write_text(json.dumps({n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt')},indent=2))
 pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
 stopped=True
 for m in ('mnt','mnt2'):
  assert run(m+'-stop',['/data1/efs/src/scripts/client.sh','stop','/data1/efs/'+m],180)==0
  assert not os.path.ismount('/data1/efs/'+m)
 for pid in pids:
  assert os.path.realpath('/proc/'+pid+'/exe')=='/data1/efs/src/efsd'
  fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'regular daemon drain')
 common=[src/'tests/live/gc_reclamation.py','--transport','tcp','--port','26590','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--root','/data1/efs','--root','/home/efs']
 codes={}
 for mode in ('direct','buffered'):
  codes['full-'+mode]=run('full-'+mode,['python3',*common,'--mode',mode,'--fresh-namespace','--posix','--continue-posix-failure','--sync-writes','--sync-write-fault','--append-sync-boundary','--append-replay-race','--append-audit','--stop-dirty-recovery','--shutdown-mutations','--wal-tail-recovery'])
 (out/'verdicts.json').write_text(json.dumps(codes,indent=2));assert all(rc==0 for rc in codes.values()),codes
 (out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
 if stopped:
  assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')),'private mounts still active; retain failure, refuse unsafe restore'
  normal=dict(env,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
  with (out/'restore.log').open('w') as log:
   rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,timeout=300).returncode
  assert rc==0
  wait(lambda:all(port(p) for p in range(17432,17436)),180,'restored listeners')
  assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
  (out/'restore-exit.txt').write_text('0\n')
