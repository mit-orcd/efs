#!/usr/bin/env python3
"""Bounded NUC integrated-code gates; preserve installed source and stores."""
import argparse,hashlib,json,os,re,signal,socket,subprocess,time,traceback
from pathlib import Path
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--resume-buffered',type=Path,help='Retain an already accepted buffered gate only when its exact images and fixture match.')
parser.add_argument('--variant',choices=('batch','clock','root-scope'),default='batch')
args=parser.parse_args()
assert not args.resume_buffered or args.variant=='batch'
base=Path('/data1/efs/devel-nuc-20261011');src=base/(args.variant+'-src');out=base/(args.variant+'-live');out.mkdir(exist_ok=False)
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
  proc=subprocess.Popen(list(map(str,args)),cwd=src,env=env,stdout=log,stderr=subprocess.STDOUT)
  try:rc=proc.wait(timeout=seconds)
  except subprocess.TimeoutExpired:
   (out/(name+'-timeout.txt')).write_text('Deadline exceeded; request Python fixture cleanup, retain failed gate.\n')
   proc.terminate();rc=proc.wait(timeout=120)
 (out/(name+'-exit.txt')).write_text(str(rc)+'\n');print('END',name,rc,flush=True);return rc
def port(p):
 try:
  with socket.create_connection(('127.0.0.1',p),.3):return True
 except OSError:return False
stopped=False
try:
 native=base/(args.variant+'-build')
 placement=base/('clock-cold' if args.variant=='root-scope' else 'data-placement' if args.variant=='clock' else 'meta-placement')
 def placed():
  assert not (placement/'failure.txt').exists(),'placement failed; do not stop regular service'
  assert not (native/'failure.txt').exists(),'native failed; do not stop regular service'
  return (placement/('run-exit.txt' if args.variant=='root-scope' else 'restore-exit.txt')).exists()
 wait(placed,7200,'placement restoration')
 def built():
  assert not (native/'failure.txt').exists(),'native failed; do not stop regular service'
  return (native/'run-exit.txt').exists()
 wait(built,7200,'native suite')
 assert (native/'run-exit.txt').read_text().strip()=='0','native failed; do not stop regular service'
 if args.variant=='root-scope':
  assert (placement/'run-exit.txt').read_text().strip()=='0'
  expected=json.loads((native/'provenance.json').read_text())['core_sha256']
  core={str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
  core['Makefile']=hashlib.sha256((src/'Makefile').read_bytes()).hexdigest()
  assert core==expected,'root candidate changed after native gate'
  accepted=json.loads((base/'clock-install/images.json').read_text())
  assert all(hashlib.sha256((Path('/data1/efs/src')/n).read_bytes()).hexdigest()==v['sha256'] for n,v in accepted.items())
 images={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt')}
 (out/'binaries.json').write_text(json.dumps(images,indent=2))
 codes={}
 if args.resume_buffered:
  prior=args.resume_buffered.resolve()
  assert prior.parent==base and prior.name=='batch-live-initial-failed'
  assert (prior/'restore-exit.txt').read_text().strip()=='0'
  assert (prior/'full-buffered-exit.txt').read_text().strip()=='0'
  log=(prior/'full-buffered.log').read_text()
  evidence=Path(re.search(r'^Evidence directory: (.+)$',log,re.M)[1])
  assert evidence.parent==Path('/data1/efs') and evidence.name.startswith('gc-buffered-')
  manifest=json.loads((evidence/'manifest.json').read_text())
  assert manifest['mode']=='buffered'
  assert manifest['test_script_sha256']==hashlib.sha256((src/'tests/live/gc_reclamation.py').read_bytes()).hexdigest()
  assert manifest['binary_sha256']=={n:v['sha256'] for n,v in images.items()}
  (out/'inherited-buffered.json').write_text(json.dumps(dict(previous=str(prior),evidence=str(evidence),manifest=manifest,log_sha256=hashlib.sha256((prior/'full-buffered.log').read_bytes()).hexdigest()),indent=2))
  (out/'full-buffered-exit.txt').write_text('0\n')
  codes['full-buffered']=0
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
 for mode in (('direct',) if args.resume_buffered else ('direct','buffered')):
  codes['full-'+mode]=run('full-'+mode,['python3',*common,'--mode',mode,'--append-burst','2000','--fresh-namespace','--posix','--continue-posix-failure','--sync-writes','--sync-write-fault','--append-sync-boundary','--append-replay-race','--append-audit','--stop-dirty-recovery','--shutdown-mutations','--wal-tail-recovery'])
 (out/'verdicts.json').write_text(json.dumps(codes,indent=2));assert all(rc==0 for rc in codes.values()),codes
 (out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
finally:
 if stopped:
  try:
   assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')),'private mounts still active; retain failure, refuse unsafe restore'
   normal=dict(env,EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
   with (out/'restore.log').open('w') as log:
    rc=subprocess.run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=log,stderr=subprocess.STDOUT,timeout=360).returncode
   assert rc==0
   wait(lambda:all(port(p) for p in range(17432,17436)),180,'restored listeners')
   assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
   if args.variant=='root-scope':
    # Restore the accepted append engine and verify every payload checked at
    # its cold gate. This fixture never installs the root candidate.
    assert all(hashlib.sha256((Path('/data1/efs/src')/n).read_bytes()).hexdigest()==v['sha256'] for n,v in accepted.items())
    checked=json.loads((base/'clock-cold/after.json').read_text())['payloads']
    for row in checked:assert hashlib.sha256(Path(row['path']).read_bytes()).hexdigest()==row['sha256']
    (out/'restored-preserved-payloads.json').write_text(json.dumps(checked,indent=2))
   (out/'restore-exit.txt').write_text('0\n')
  except BaseException:
   failure=traceback.format_exc()
   (out/'restore-failure.txt').write_text(failure)
   if not (out/'failure.txt').exists():(out/'failure.txt').write_text(failure)
   (out/'restore-exit.txt').write_text('1\n')
   (out/'run-exit.txt').write_text('1\n')
   raise
