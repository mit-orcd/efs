#!/usr/bin/env python3
"""Qualified W97 ABBA comparison on an identical accepted W52 core; private stores only."""
import gzip,hashlib,json,os,re,signal,socket,subprocess,time,traceback
from pathlib import Path
from nuc_sync_counters import SyncCounters
session=Path('/data1/efs/devel-nuc-20261011')
base=session/'catchup-w97-abba';base.mkdir(exist_ok=False)
installed=Path('/data1/efs/src');src=None;identity=None
port=26770;meta_roots=[];mount=base/'mnt';mount.mkdir()
servers=[];handles=[];loader=None;counters=None;profilers=[]
flags=dict(EFS_MD_RAFT_N='3',EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_COMMANDS_SERVER='0',EFS_COMMANDS='0',EFS_TRANSPORT='tcp',EFS_BATCH_OBS='1')
env=dict(os.environ,**flags)
def accepted(name):
 folder=session/name
 assert not (folder/'failure.txt').exists(),name+' failed'
 marker=folder/'run-exit.txt'
 if not marker.exists():return False
 assert marker.read_text().strip()=='0',name+' not accepted'
 return True

def hashfile(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def verify_preserved():
 rows=json.loads((session/'catchup-service-cold/after.json').read_text())['payloads']
 for row in rows:assert hashfile(row['path'])==row['sha256'],row['path']
 return rows

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
 commands=[('perf',['perf','record','-F','99','-g','--call-graph','fp','-p',','.join(str(p['pid']) for p in processes),'-o',str(profile/'perf.data'),'--','sleep','30'],0),('strace',['timeout','--signal=INT','10s','strace','-f','-yy','-ttt','-T','-e','trace=fsync,fdatasync',*[x for p in processes if p['kind']=='efsd' for x in ('-p',str(p['pid']))]],124)]
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
  args=[src/'efsd','--node-id',n,'--addr','127.0.0.1','--port',port+n-1,'--storage',root,'--quota','60G','--direct-io']
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
 start_private(roots,cell,'');run([src/'efs-mgmt','raft-mkfs',f'127.0.0.1:{port}'],stdout=(cell/'mkfs.log').open('w'));pin_leaders(roots,cell);mount_private(cell,'');status()
 with (cell/'loader.log').open('w') as out:
  loader=subprocess.Popen(['python3',base.parent/'nuc_balanced_tiny_load.py',mount/'tiny',cell],stdout=out,stderr=out)
 wait(lambda:(cell/'ready.json').exists() if loader.poll() is None else (_ for _ in ()).throw(RuntimeError('loader prep failed')),900,'balanced prep')
 leaders={r['group']:r['leader'] for r in status()};assert set(leaders.values())=={1}
 time.sleep(30) # Same fixed settle interval; preparation is outside timed work.
 servermap={n:dict(pid=p.pid) for n,p in enumerate(servers,1)}
 counters=SyncCounters(cell,servermap);counters.command('enable');(cell/'GO').touch()
 wait(lambda:(cell/'load-start.json').exists(),10,'load start');origin=json.loads((cell/'load-start.json').read_text())['mono_us']/1e6
 with (cell/'samples.jsonl').open('w') as out:
  for i in range(121):
   time.sleep(max(0,origin+i*5-time.monotonic()));assert loader.poll() is None,'loader ended/cap hit'
   current=status();assert all(r['leader']==leaders[r['group']] for r in current),'leader changed in timed window'
   row=dict(seconds=time.monotonic()-origin,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),progress=progress(cell),diskstats=Path('/proc/diskstats').read_text(),host_stat=Path('/proc/stat').read_text(),meminfo=Path('/proc/meminfo').read_text(),raft=current,processes={str(p.pid):{n:Path(f'/proc/{p.pid}/{n}').read_text() for n in ('status','io','stat')} for p in servers})
   if i%12==0:headroom()
   out.write(json.dumps(row)+'\n');out.flush()
   if i==90:start_profiles(cell)
   if i==97:finish_profiles()
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


def pin_leaders(roots,cell):
 # Normal single-node restart before admission; two voters always remain.
 # Abort rather than accepting a different placement or altering election code.
 attempts=[]
 for attempt in range(12):
  rows=status();leaders={r['group']:r['leader'] for r in rows}
  if len(leaders)==2 and set(leaders.values())=={1}:
   (cell/'leader-placement.json').write_text(json.dumps(dict(leaders=leaders,attempts=attempts),indent=2));return
  other=next((n for n in sorted(set(leaders.values())) if n in (2,3)),None)
  assert other is not None,rows
  cuts={r['group']:r['commit'] for r in rows if r['role']=='LEADER'}
  wait(lambda:len(cuts)==2 and all(r['applied']>=cuts[r['group']] for r in status()),120,'pre-election catchup')
  old=servers[other-1];old.terminate();assert old.wait(timeout=180)==0
  time.sleep(5)
  h=(cell/f'n{other}-election-{attempt}.log').open('ab');handles.append(h)
  cmd=[src/'efsd','--node-id',other,'--addr','127.0.0.1','--port',port+other-1,'--storage',roots[other-1],'--meta-storage',meta_roots[other-1],'--quota','60G','--direct-io','--join',f'127.0.0.1:{port}']
  servers[other-1]=subprocess.Popen(list(map(str,cmd)),env=env,stdout=h,stderr=h)
  wait(lambda:listening(port+other-1) if servers[other-1].poll() is None else (_ for _ in ()).throw(RuntimeError('election restart exited')),120,'election restart')
  time.sleep(5);attempts.append(dict(attempt=attempt,restarted=other,before=leaders,after=status()))
 (cell/'leader-placement-failure.json').write_text(json.dumps(attempts,indent=2))
 raise RuntimeError('Could not match both leaders to node1 in bounded pre-timed restarts')


service_stopped=False
try:
 wait(lambda:all(accepted(n) for n in ('catchup-base-build','catchup-root-build','catchup-base-live','catchup-install','catchup-service-cold','catchup-root-live')),43200,'full matched native/live/cold acceptance')
 baseline=json.loads((session/'catchup-base-build/provenance.json').read_text())['core_sha256']
 candidate=json.loads((session/'catchup-root-build/provenance.json').read_text())['core_sha256']
 assert set(baseline)==set(candidate)
 differences=sorted(k for k in baseline if baseline[k]!=candidate[k])
 assert differences==['src/server/packed_fence.h','src/server/store.c'],differences
 regular=json.loads((session/'catchup-install/images.json').read_text())
 assert all(hashfile(installed/n)==v['sha256'] for n,v in regular.items())
 verify_preserved()
 procs=subprocess.check_output(['ps','-eo','pid,args'],text=True)
 assert not any(re.search(r'^\s*\d+\s+(?:python3|/usr/bin/python3) .*?(tiny_files.py|balanced-load.py|drive-load.py|nuc_balanced_tiny_load.py)',x) for x in procs.splitlines()),'generator active'
 assert all(not listening(p) for p in range(port,port+3))
 pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
 assert all(hashfile(Path('/proc',p,'exe'))==regular['efsd']['sha256'] for p in pids)
 (base/'comparison.json').write_text(json.dumps(dict(core_differences=differences,flags=flags,seconds_per_cell=600,order=['base','root','root','base'],media=['sata','nvme'],metadata='SATA',leader_node=1,windows=[0,60,540,600],limits='Filesystem/device-path comparison; no hardware ceiling or NAND/cache attribution.'),indent=2))
 service_stopped=True
 for m in ('mnt','mnt2'):
  run([installed/'scripts/client.sh','stop','/data1/efs/'+m],stdout=(base/(m+'-stop.log')).open('w'),stderr=subprocess.STDOUT)
  assert not os.path.ismount('/data1/efs/'+m)
 for pid in pids:
  fd=os.pidfd_open(int(pid));signal.pidfd_send_signal(fd,signal.SIGTERM);os.close(fd)
 wait(lambda:all(not Path('/proc',p).exists() for p in pids),180,'regular drain')
 assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
 for medium in ('sata','nvme'):
  for iteration,variant in enumerate(('base','root','root','base')):
   src=session/('catchup-'+variant+'-src')
   manifest=json.loads((session/('catchup-'+variant+'-build')/'provenance.json').read_text())['core_sha256']
   actual={str(p.relative_to(src)):hashfile(p) for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')};actual['Makefile']=hashfile(src/'Makefile')
   assert actual==manifest
   identity=json.loads((session/('catchup-'+variant+'-build')/'images.json').read_text())
   assert all(hashfile(src/n)==v['sha256'] for n,v in identity.items())
   tag=f'{medium}-{iteration}-{variant}'
   storage=Path('/data1/efs' if medium=='sata' else '/home/efs')/('w97-catchup-abba-data-'+tag)
   metadata=Path('/data1/efs')/('w97-catchup-abba-meta-'+tag)
   cell_test(tag,storage,metadata);time.sleep(30)
 (base/'tests-exit.txt').write_text('0\n')
except BaseException:
 (base/'failure.txt').write_text(traceback.format_exc());raise
finally:
 try:stop_private()
 finally:
  if service_stopped:
   assert not servers and not os.path.ismount(mount)
   assert subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==1
   assert all(hashfile(installed/n)==v['sha256'] for n,v in regular.items())
   normal=dict(env,EFS_MD_RAFT_N='4',EFS_BATCH_OBS='0',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
   with (base/'restore.log').open('w') as out:run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=out,stderr=subprocess.STDOUT,timeout=360)
   wait(lambda:all(listening(p) for p in range(17432,17436)),180,'restored listeners')
   assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
   assert all(hashfile(installed/n)==v['sha256'] for n,v in regular.items())
   (base/'restored-payloads.json').write_text(json.dumps(verify_preserved(),indent=2))
   (base/'restore-exit.txt').write_text('0\n')
 if not (base/'failure.txt').exists() and (base/'restore-exit.txt').exists():(base/'run-exit.txt').write_text('0\n')
