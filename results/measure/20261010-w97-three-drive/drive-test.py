"""Two fresh three-node clusters, one NVMe-only and one SATA-only, preserving service stores."""
import gzip,hashlib,json,os,re,signal,socket,subprocess,time
from pathlib import Path
from w93_perf import SyncCounters
base=Path(__file__).resolve().parent;src=Path('/data1/efs/src');port=25670
flags=dict(EFS_MD_RAFT_N='3',EFS_RAFT_FLOW='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_APPLIED_INTERVAL_MS='10',EFS_COMMANDS_SERVER='0',EFS_COMMANDS='0',EFS_TRANSPORT='tcp',EFS_BATCH_OBS='1')
env=dict(os.environ,**flags);mount=base/'mnt';mount.mkdir(exist_ok=True);servers=[];handles=[];loader=None;counters=None
# Record actual identities; installed engine is frozen for both cells.
identity={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt')}
assert identity['efsd']['version'].startswith('efsd 6b8f05ac ')
previous=json.loads(Path('/data1/efs/w97-latest-20261010/binary-provenance.json').read_text())
assert all(v['sha256']==previous[n]['sha256'] for n,v in identity.items())
(base/'identity.json').write_text(json.dumps(dict(binaries=identity,flags=flags,seconds_per_cell=1200,port=port),indent=2))
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
 if os.path.ismount(mount):run([src/'scripts/client.sh','stop',mount],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 for p in servers:
  if p.poll() is None:p.terminate()
 for p in servers:p.wait(timeout=180)
 servers=[]
 for h in handles:h.close()
 handles=[]
def start_private(roots,cell,suffix):
 assert not servers
 for n,root in enumerate(roots,1):
  root.mkdir(exist_ok=True);h=(cell/f'n{n}{suffix}.log').open('ab');handles.append(h)
  args=[src/'efsd','--node-id',n,'--addr','127.0.0.1','--port',port+n-1,'--storage',root,'--quota','80G','--direct-io']
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

def cell_test(tag,storage):
 global loader,counters
 cell=base/tag;cell.mkdir();storage.mkdir();roots=[storage/f'n{n}' for n in range(1,4)]
 placement={str(p):subprocess.check_output(['findmnt','-T',str(storage),'-no','SOURCE,FSTYPE'],text=True).strip() for p in roots}
 (cell/'storage-placement.json').write_text(json.dumps(placement,indent=2));headroom()
 start_private(roots,cell,'');run([src/'efs-mgmt','raft-mkfs',f'127.0.0.1:{port}'],stdout=(cell/'mkfs.log').open('w'));mount_private(cell,'');status()
 with (cell/'loader.log').open('w') as out:
  loader=subprocess.Popen(['python3',base/'drive-load.py',mount/'tiny',cell],stdout=out,stderr=out)
 wait(lambda:(cell/'ready.json').exists() if loader.poll() is None else (_ for _ in ()).throw(RuntimeError('loader prep failed')),900,'balanced prep')
 time.sleep(30) # Same fixed settle interval; preparation is outside timed work.
 servermap={n:dict(pid=p.pid) for n,p in enumerate(servers,1)}
 counters=SyncCounters(cell,servermap);counters.command('enable');(cell/'GO').touch()
 wait(lambda:(cell/'load-start.json').exists(),10,'load start');origin=json.loads((cell/'load-start.json').read_text())['mono_us']/1e6
 with (cell/'samples.jsonl').open('w') as out:
  for i in range(241):
   time.sleep(max(0,origin+i*5-time.monotonic()));assert loader.poll() is None,'loader ended/cap hit'
   row=dict(seconds=time.monotonic()-origin,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),progress=progress(cell),diskstats=Path('/proc/diskstats').read_text(),host_stat=Path('/proc/stat').read_text(),meminfo=Path('/proc/meminfo').read_text(),raft=status(),processes={str(p.pid):{n:Path(f'/proc/{p.pid}/{n}').read_text() for n in ('status','io','stat')} for p in servers})
   if i%12==0:headroom()
   out.write(json.dumps(row)+'\n');out.flush()
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
 (cell/'complete.json').write_text(json.dumps(dict(tag=tag,files=sum(final['counts']),errors=final['errors'],cold_checks=64,storage=str(storage),start_mono=origin,seconds=1200),indent=2));print(tag,'PASS',sum(final['counts']),flush=True)

# Refuse interfering generators; stop only the identified service cluster.
procs=subprocess.check_output(['ps','-eo','pid,args'],text=True)
assert not any(re.search(r'^(?:\s*\d+\s+)(?:python3|/usr/bin/python3) .*?(tiny_files.py|balanced-load.py|drive-load.py)',x) for x in procs.splitlines()),'generator active'
for p in range(port,port+3):assert not listening(p),'private port occupied'
service_stopped=False
try:
 service_stopped=True
 for m in ('/data1/efs/mnt','/data1/efs/mnt2'):run([src/'scripts/client.sh','stop',m],stdout=(base/(Path(m).name+'-stop.log')).open('w'),stderr=subprocess.STDOUT);assert not os.path.ismount(m)
 pids=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();assert len(pids)==4
 for pid in pids:os.kill(int(pid),signal.SIGTERM)
 service_stopped=True
 wait(lambda:all(not Path('/proc',pid).exists() for pid in pids),180,'service drain')
 assert not subprocess.run(['pgrep','-x','efsd'],stdout=subprocess.DEVNULL).returncode==0,'unrelated server active'
 cell_test('nvme',Path('/home/efs/w97-three-nvme-v2-20261010'))
 time.sleep(60)
 cell_test('sata',Path('/data1/efs/w97-three-sata-v2-20261010'))
 (base/'tests-exit.txt').write_text('0\n')
except BaseException:
 import traceback
 (base/'failure.txt').write_text(traceback.format_exc())
 raise
finally:
 try:stop_private()
 finally:
  if service_stopped:
   normal=dict(env,EFS_MD_RAFT_N='4',EFS_BATCH_OBS='0',EFS_CLUSTER='nuc',EFS_DIRECT_IO='1')
   with (base/'restore.log').open('w') as out:run(['bash','/data1/efs/w97-latest-20261010/start-preserved.sh'],env=normal,stdout=out,stderr=subprocess.STDOUT)
   wait(lambda:all(listening(p) for p in range(17432,17436)),120,'restored service listeners')
   run(['python3','/data1/efs/w97-latest-20261010/final-state.py'],stdout=(base/'final-service-state.log').open('w'),stderr=subprocess.STDOUT)
   (base/'restore-exit.txt').write_text('0\n')
