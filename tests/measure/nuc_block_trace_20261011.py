#!/usr/bin/env python3
"""Bounded NUC integrated-code gates; preserve installed source and stores."""
import hashlib,json,os,signal,socket,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');src=base/'candidate';out=base/'block-traces';out.mkdir(exist_ok=False)
env=dict(os.environ,EFS_COMMANDS='0',EFS_COMMANDS_SERVER='0',EFS_TRANSPORT='tcp',EFS_MD_RAFT_N='4',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_BATCH_OBS='0')
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
stopped=False;bench=None
try:
 wait(lambda:(base/'append-comparison-v3/restore-exit.txt').exists(),5400,'append comparison restoration')
 import datetime
 remaining=datetime.datetime(2026,10,11,14,56,19,tzinfo=datetime.timezone.utc).timestamp()-time.time()
 if remaining<20*60:
  (out/'NOTRUN.json').write_text(json.dumps(dict(reason='insufficient remaining session time for drive matrix and preserved-store restoration',remaining_seconds=remaining)))
  raise SystemExit(0)
 seconds=240 if remaining>=32*60 else 120 if remaining>=24*60 else 60
 (out/'duration.json').write_text(json.dumps(dict(seconds_per_cell=seconds,remaining_seconds_at_launch=remaining)))
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
 for order,drive in enumerate(('nvme','sata','sata','nvme')):
  name=f'{order}-{drive}';cell=out/name
  root=(Path('/home/efs') if drive=='nvme' else Path('/data1/efs'))/f'w97-block-20261011-{name}'
  with (out/(name+'.log')).open('w') as log:
   bench=subprocess.Popen(['python3',str(src/'tests/measure/nuc_durable_io.py'),str(root),str(cell),'--mode','written-ring','--seconds',str(seconds)],stdout=log,stderr=subprocess.STDOUT,env=env)
   wait(lambda:(cell/'ready.json').exists() or bench.poll() is not None,180,'written extents ready')
   assert bench.poll() is None,'benchmark failed before timing'
   (cell/'policy.json').write_text(json.dumps({p:Path(p).read_text().strip() for p in ('/sys/block/sda/queue/write_cache','/sys/block/nvme0n1/queue/write_cache','/sys/block/sda/queue/fua','/sys/block/nvme0n1/queue/fua','/sys/block/sda/queue/scheduler','/sys/block/nvme0n1/queue/scheduler','/sys/class/nvme/nvme0/firmware_rev','/sys/module/nvme_core/parameters/default_ps_max_latency_us')},indent=2))
   ready=json.loads((cell/'ready.json').read_text());origin=ready['monotonic']
   for phase,delay in (('early',5 if seconds>=120 else 3),('late',seconds-35 if seconds>=120 else seconds-30)):
    wait(lambda:time.monotonic()-origin>=delay,250,'trace window')
    assert bench.poll() is None,'writer ended before trace window'
    physical='nvme0n1' if drive=='nvme' else 'sda'
    major,minor=map(int,Path('/sys/block',physical,'dev').read_text().strip().split(':'))
    device=(major<<20)|minor
    command=['perf','record','-a','-e','block:block_rq_issue','--filter',f'dev == {device}','-e','block:block_rq_complete','--filter',f'dev == {device}','-o',str(cell/(phase+'.data')),'--','sleep',str(20 if seconds>=120 else 10)]
    with (cell/(phase+'-record.log')).open('w') as trace:
     rc=subprocess.run(command,stdout=trace,stderr=subprocess.STDOUT,timeout=45).returncode
    (cell/(phase+'-record-exit.txt')).write_text(str(rc)+'\n');assert rc==0
    with (cell/(phase+'-events.txt')).open('w') as trace:
     rc=subprocess.run(['perf','script','--ns','-i',str(cell/(phase+'.data'))],stdout=trace,stderr=subprocess.STDOUT,timeout=90).returncode
    (cell/(phase+'-script-exit.txt')).write_text(str(rc)+'\n');assert rc==0
   rc=bench.wait(timeout=100)
  codes[name]=rc;(out/'verdicts.json').write_text(json.dumps(codes,indent=2));assert rc==0
 (out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
 if bench is not None and bench.poll() is None:
  fd=os.pidfd_open(bench.pid);signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
  bench.wait(timeout=30)
 if stopped:
  assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')),'private mounts still active; retain failure, refuse unsafe restore'
  normal=dict(env,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
  with (out/'restore.log').open('w') as log:
   rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,timeout=300).returncode
  assert rc==0
  wait(lambda:all(port(p) for p in range(17432,17436)),180,'restored listeners')
  assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
  (out/'restore-exit.txt').write_text('0\n')
