"""Matched three-voter NVMe-data cells with NVMe/SATA/NVMe metadata; preserve service stores."""
import argparse,gzip,hashlib,json,os,re,signal,socket,subprocess,sys,time
from pathlib import Path
from nuc_sync_counters import SyncCounters
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vary',choices=('metadata','data'),default='metadata')
parser.add_argument('--accepted-baseline',action='store_true',help='For data placement, use the restored accepted 108afa3e engine after a failed candidate gate; do not inherit candidate acceptance.')
parser.add_argument('--profile-late',action='store_true',help='Profile each cell at 450 seconds for 30 seconds, with 10 seconds of daemon strace; first/last comparison windows remain outside profiling.')
args=parser.parse_args();session=Path('/data1/efs/devel-nuc-20261011')
assert not args.accepted_baseline or args.vary=='data'
if args.accepted_baseline:
 end=time.monotonic()+1800
 while not (session/'batch-live/restore-exit.txt').exists():
  assert time.monotonic()<end,'candidate cleanup and accepted service restoration timeout'
  time.sleep(1)
 assert (session/'batch-live/restore-exit.txt').read_text().strip()=='0'
 assert (session/'batch-live/failure.txt').exists(),'baseline resumption requires retained failed candidate evidence'
 assert (session/'batch-live/full-direct-exit.txt').read_text().strip()!='0'
 assert not (session/'batch-install/run-exit.txt').exists(),'candidate installed; refuse baseline identity'
elif args.vary=='data':
 end=time.monotonic()+14400
 while not (session/'w52-fixed-20000/run-exit.txt').exists():
  assert time.monotonic()<end,'long append prerequisite timeout'
  assert not any((session/p/'failure.txt').exists() for p in ('batch-fixture-retry','w52-fixed-20000')),'prerequisite failed'
  time.sleep(1)
 assert (session/'w52-fixed-20000/run-exit.txt').read_text().strip()=='0'
 assert (session/'batch-install/run-exit.txt').read_text().strip()=='0'
 while not any((session/'w52-long-profile'/name).exists() for name in ('run-exit.txt','failure.txt')):
  assert time.monotonic()<end,'profile cleanup prerequisite timeout'
  time.sleep(1)
base=session/('meta-placement' if args.vary=='metadata' else 'data-placement');base.mkdir(exist_ok=False)
src=Path('/data1/efs/src');port=26770;meta_roots=[]
flags=dict(EFS_MD_RAFT_N='3',EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_COMMANDS_SERVER='0',EFS_COMMANDS='0',EFS_TRANSPORT='tcp',EFS_BATCH_OBS='1')
env=dict(os.environ,**flags);mount=base/'mnt';mount.mkdir(exist_ok=True);servers=[];handles=[];loader=None;counters=None;profilers=[]
# Record actual identities; installed engine is frozen for both cells.
identity={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt')}
baseline=args.vary=='metadata' or args.accepted_baseline
assert identity['efsd']['version'].startswith('efsd '+('108afa3e-nuc' if baseline else '4afc063c-nuc')+' ')
previous=json.loads((session/('deployment-20261011/installed-images.json' if baseline else 'batch-install/images.json')).read_text())
assert all(v['sha256']==previous[n]['sha256'] for n,v in identity.items())
drives=('nvme','sata','nvme') if args.vary=='metadata' else ('sata','nvme','sata')
(base/'identity.json').write_text(json.dumps(dict(binaries=identity,flags=flags,seconds_per_cell=600,port=port,vary=args.vary,accepted_baseline=args.accepted_baseline,profile_late=args.profile_late,cells=[f'{i}-{d}' for i,d in enumerate(drives)]),indent=2))
def run(args,**kw):return subprocess.run(list(map(str,args)),check=True,**kw)
def rpc(command,n):return subprocess.check_output([str(src/'efs-mgmt'),command,f'127.0.0.1:{port+n-1}'],text=True,timeout=15)
def wait(fn,seconds,label):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if fn():return
  time.sleep(.3)
 raise RuntimeError(label+' timed out')
def listening(p):
 try:
  with socket.create_connection(('127.0.0.1',p),timeout=.3):return True
 except OSError:return False
def stop_private():
 global loader,counters,servers,handles
 if loader is not None and loader.poll() is None:loader.send_signal(signal.SIGUSR1);loader.wait(timeout=180)
 if counters is not None:counters.stop(False);counters=None
 finish_profiles(False)
 if os.path.ismount(mount):run([src/'scripts/client.sh','stop',mount],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 for p in servers:
  if p.poll() is None:p.terminate()
 for p in servers:p.wait(timeout=180)
 servers=[]
 for h in handles:h.close()
 handles=[]
def start_profiles(cell):
 assert not profilers
 processes=[dict(pid=p.pid,kind='efsd') for p in servers]
 for proc in Path('/proc').iterdir():
  if not proc.name.isdigit():continue
  try:
   if os.readlink(proc/'exe')==str(src/'efs-fuse') and str(mount).encode() in (proc/'cmdline').read_bytes().split(b'\0'):
    processes.append(dict(pid=int(proc.name),kind='efs-fuse'))
  except (FileNotFoundError,ProcessLookupError,PermissionError):continue
 assert len(processes)==4 and sum(p['kind']=='efs-fuse' for p in processes)==1
 for p in processes:assert hashlib.sha256(Path('/proc',str(p['pid']),'exe').read_bytes()).hexdigest()==identity[p['kind']]['sha256']
 profile=cell/'late-profile';profile.mkdir()
 (profile/'window.json').write_text(json.dumps(dict(start_utc=time.strftime('%FT%TZ',time.gmtime()),start_monotonic=time.monotonic(),processes=processes,limits='Matched instrumented window near 450 seconds in every cell; exclude it from unprofiled speed comparisons. Not a hardware ceiling.'),indent=2))
 commands=[('perf',['perf','record','-F','99','-g','--call-graph','fp','-p',','.join(str(p['pid']) for p in processes),'-o',str(profile/'perf.data'),'--','sleep','30'],0),('strace',['timeout','--signal=INT','10s','strace','-f','-c','-w',*[x for p in processes if p['kind']=='efsd' for x in ('-p',str(p['pid']))]],124)]
 for name,cmd,expected in commands:
  log=(profile/(name+'.log')).open('w');job=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT)
  profilers.append((profile,name,job,log,expected))
def finish_profiles(validate=True):
 jobs=list(profilers);profilers.clear()
 errors=[]
 for profile,name,job,log,expected in jobs:
  if not validate and job.poll() is None:job.send_signal(signal.SIGINT)
  try:rc=job.wait(timeout=45)
  except subprocess.TimeoutExpired:job.terminate();rc=job.wait(timeout=15)
  log.close();(profile/(name+'-exit.txt')).write_text(str(rc)+'\n')
  if validate and rc!=expected:errors.append((name,rc))
  if name=='perf' and rc==0:
   with (profile/'flat.txt').open('w') as out:run(['perf','report','--stdio','--no-children','--percent-limit','0.5','-i',profile/'perf.data'],stdout=out,stderr=subprocess.STDOUT,timeout=60)
   with (profile/'callers.txt').open('w') as out:run(['perf','report','--stdio','--percent-limit','0.5','-i',profile/'perf.data'],stdout=out,stderr=subprocess.STDOUT,timeout=60)
 assert not errors,errors
def start_private(roots,cell,suffix):
 assert not servers
 for n,root in enumerate(roots,1):
  root.mkdir(exist_ok=True);h=(cell/f'n{n}{suffix}.log').open('ab');handles.append(h)
  args=[src/'efsd','--node-id',n,'--addr','127.0.0.1','--port',port+n-1,'--storage',root,'--quota','80G','--direct-io']
  args+=['--meta-storage',meta_roots[n-1]]
  if n>1:args+=['--join',f'127.0.0.1:{port}']
  p=subprocess.Popen(list(map(str,args)),env=env,stdout=h,stderr=h);servers.append(p)
  wait(lambda:listening(port+n-1) if p.poll() is None else (_ for _ in ()).throw(RuntimeError('server exited')),120,'private listener')
 time.sleep(5)
def mount_private(cell,suffix):
 h=(cell/f'fuse{suffix}.log').open('ab');handles.append(h)
 run([src/'scripts/client.sh',f'127.0.0.1:{port}',mount],env=env,stdout=h,stderr=h)
def progress(cell):
 p=cell/'progress.jsonl'
 if not p.exists():return dict(counts=[0]*8)
 for line in reversed(p.read_text().splitlines()):
  try:return json.loads(line)
  except ValueError:pass
 return dict(counts=[0]*8)
def status():
 rows=[]
 for n in range(1,4):
  raw=rpc('raft-status',n)
  for m in re.finditer(r'group (\d+) hosted=(\d+) role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+) voters=(0x[0-9a-f]+)',raw):
   g,hosted,role,leader,term,commit,applied,voters=m.groups();rows.append(dict(node=n,group=int(g),hosted=int(hosted),role=role,leader=int(leader),term=int(term),commit=int(commit),applied=int(applied),voters=int(voters,16)))
 assert len(rows)==6 and all(r['hosted']==1 and r['voters']==7 for r in rows),rows
 return rows
def headroom():
 assert min(__import__('shutil').disk_usage(p).free for p in ('/data1/efs','/home/efs'))>80*2**30
 assert int(re.search(r'^MemAvailable:\s+(\d+)',Path('/proc/meminfo').read_text(),re.M)[1])>3*2**20

def cell_test(tag,storage,metadata):
 global loader,counters,meta_roots
 cell=base/tag;cell.mkdir();storage.mkdir();metadata.mkdir();roots=[storage/f'n{n}' for n in range(1,4)]
 meta_roots=[metadata/f'n{n}' for n in range(1,4)]
 for root in meta_roots:root.mkdir()
 placement={str(p):subprocess.check_output(['findmnt','-T',str(storage),'-no','SOURCE,FSTYPE'],text=True).strip() for p in roots}
 (cell/'storage-placement.json').write_text(json.dumps(dict(data=placement,metadata={str(p):subprocess.check_output(['findmnt','-T',str(p),'-no','SOURCE,FSTYPE'],text=True).strip() for p in meta_roots}),indent=2));headroom()
 start_private(roots,cell,'');run([src/'efs-mgmt','raft-mkfs',f'127.0.0.1:{port}'],stdout=(cell/'mkfs.log').open('w'));mount_private(cell,'');status()
 with (cell/'loader.log').open('w') as out:
  loader=subprocess.Popen(['python3',base.parent/'nuc_balanced_tiny_load.py',mount/'tiny',cell],stdout=out,stderr=out)
 wait(lambda:(cell/'ready.json').exists() if loader.poll() is None else (_ for _ in ()).throw(RuntimeError('loader prep failed')),900,'balanced prep')
 time.sleep(30) # Same fixed settle interval; preparation is outside timed work.
 servermap={n:dict(pid=p.pid) for n,p in enumerate(servers,1)}
 counters=SyncCounters(cell,servermap);counters.command('enable');(cell/'GO').touch()
 wait(lambda:(cell/'load-start.json').exists(),10,'load start');origin=json.loads((cell/'load-start.json').read_text())['mono_us']/1e6
 with (cell/'samples.jsonl').open('w') as out:
  for i in range(121):
   time.sleep(max(0,origin+i*5-time.monotonic()));assert loader.poll() is None,'loader ended/cap hit'
   row=dict(seconds=time.monotonic()-origin,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),progress=progress(cell),diskstats=Path('/proc/diskstats').read_text(),host_stat=Path('/proc/stat').read_text(),meminfo=Path('/proc/meminfo').read_text(),raft=status(),processes={str(p.pid):{n:Path(f'/proc/{p.pid}/{n}').read_text() for n in ('status','io','stat')} for p in servers})
   if i%12==0:headroom()
   out.write(json.dumps(row)+'\n');out.flush()
   if args.profile_late and i==90:start_profiles(cell)
   if args.profile_late and i==97:finish_profiles()
 counters.command('disable');counters.stop();counters=None;loader.send_signal(signal.SIGUSR1);assert loader.wait(timeout=180)==0
 final=json.loads((cell/'load-final.json').read_text());assert final['errors']==[] and max(final['counts'])<512000
 rows=status();cuts={r['group']:r['commit'] for r in rows if r['role']=='LEADER'};assert len(cuts)==2
 def caught():return all(r['applied']>=cuts[r['group']] and r['commit']>=cuts[r['group']] for r in status())
 wait(caught,120,'replica catch-up');(cell/'catch-up.json').write_text(json.dumps(dict(cuts=cuts,status=status()),indent=2))
 stop_private();start_private(roots,cell,'-cold');mount_private(cell,'-cold')
 locations=json.loads((cell/'placement.json').read_text())['groups'];checked=[]
 for w in range(8):
  group='0' if w<4 else '2';n=final['counts'][w];assert n>=8
  for i in list(range(4))+list(range(n-4,n)):
   path=Path(locations[group][(w%4)*512+i//1000]['path'])/f'f{w}-{i}';size=1+((w*65537+i)*1103515245+12345)%512;expected=(hashlib.sha256(f'{w}:{i}'.encode()).digest()*16)[:size];assert path.read_bytes()==expected;checked.append(str(path))
 (cell/'cold-verification.json').write_text(json.dumps(dict(exact_files=len(checked),paths=checked),indent=2));stop_private()
 for n in range(1,4):
  p=cell/f'n{n}.log';rows=[]
  for line in p.read_text(errors='replace').splitlines():
   if 'batch-obs {' in line:
    rows.append(json.loads(line.split('batch-obs ',1)[1]))
  (cell/f'n{n}-observations.jsonl').write_text('\n'.join(json.dumps(r) for r in rows)+'\n')
  with gzip.open(cell/f'n{n}.log.gz','wb') as out:out.write(p.read_bytes())
  p.unlink() # compress only this private observer log, never storage.
 (cell/'complete.json').write_text(json.dumps(dict(tag=tag,files=sum(final['counts']),errors=final['errors'],cold_checks=64,storage=str(storage),start_mono=origin,seconds=600),indent=2));print(tag,'PASS',sum(final['counts']),flush=True)

# Await the owned preceding workload. Never overlap timing cells.
wait(lambda:Path('/data1/efs/devel-nuc-20261011/resume-20261011/run-exit.txt').exists(),1200,'preceding W97 workload')
assert Path('/data1/efs/devel-nuc-20261011/resume-20261011/run-exit.txt').read_text().strip()=='0'
# Refuse interfering generators; stop only the identified service cluster.
procs=subprocess.check_output(['ps','-eo','pid,args'],text=True)
assert not any(re.search(r'^(?:\s*\d+\s+)(?:python3|/usr/bin/python3) .*?(tiny_files.py|balanced-load.py|drive-load.py)',x) for x in procs.splitlines()),'generator active'
for p in range(port,port+3):assert not listening(p),'private port occupied'
if args.accepted_baseline:
 for sample in json.loads((session/'final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']:
  for m in ('mnt','mnt2'):assert hashlib.sha256((Path('/data1/efs',m)/sample['path']).read_bytes()).hexdigest()==sample['sha256']
service_stopped=False
try:
 service_stopped=True
 for m in ('/data1/efs/mnt','/data1/efs/mnt2'):run([src/'scripts/client.sh','stop',m],stdout=(base/(Path(m).name+'-stop.log')).open('w'),stderr=subprocess.STDOUT);assert not os.path.ismount(m)
 pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
 for pid in pids:
  assert os.path.realpath('/proc/'+pid+'/exe')==str(src/'efsd')
  fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 service_stopped=True
 wait(lambda:all(not Path('/proc',pid).exists() for pid in pids),180,'service drain')
 assert not subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==0,'unrelated server active'
 for iteration,drive in enumerate(drives):
  prefix='w97-meta-placement' if args.vary=='metadata' else 'w97-data-placement'
  data=Path('/home/efs' if args.vary=='metadata' or drive=='nvme' else '/data1/efs')/f'{prefix}-data-20261011-{iteration}'
  metadata=Path('/home/efs' if args.vary=='metadata' and drive=='nvme' else '/data1/efs')/f'{prefix}-meta-20261011-{iteration}'
  cell_test(f'{iteration}-{drive}',data,metadata)
  time.sleep(30)
 (base/'tests-exit.txt').write_text('0\n')
except BaseException:
 import traceback
 (base/'failure.txt').write_text(traceback.format_exc())
 raise
finally:
 try:stop_private()
 finally:
  if service_stopped:
   assert not servers and not os.path.ismount(mount),'private service cleanup incomplete'
   normal=dict(env,EFS_MD_RAFT_N='4',EFS_BATCH_OBS='0',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
   with (base/'restore.log').open('w') as out:run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=out,stderr=subprocess.STDOUT)
   wait(lambda:all(listening(p) for p in range(17432,17436)),120,'restored service listeners')
   assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
   for n,v in identity.items():assert hashlib.sha256((src/n).read_bytes()).hexdigest()==v['sha256']
   for sample in json.loads(Path('/data1/efs/devel-nuc-20261011/final-service.json').read_text())['existing_payload_samples_equal_on_both_mounts']:
    for m in ('mnt','mnt2'):assert hashlib.sha256((Path('/data1/efs',m)/sample['path']).read_bytes()).hexdigest()==sample['sha256']
   if args.vary=='data' and not args.accepted_baseline:
    sys.path.insert(0,str(src/'tests/live'));import append_burst
    hashes={m:append_burst.verify(Path('/data1/efs',m,'w52-fixed-20000-20261011','append-burst'),20000) for m in ('mnt','mnt2')}
    assert len(set(hashes.values()))==1
    (base/'preserved-long-append-cold.json').write_text(json.dumps(dict(records=20000,record_bytes=4096,hashes=hashes,limits='Exact cold client/daemon restart bytes; not a hardware power-loss gate.'),indent=2))
   (base/'restore-exit.txt').write_text('0\n')
