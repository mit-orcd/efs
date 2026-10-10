"""Frozen-binary A/B: default cohorts/10ms versus overlap+noop cohorts/100ms.

The treatment is a bundle; it does not isolate individual switches. Fixed
read probes, equal group workers and ABBA order address earlier confounders.
"""
import atexit,gzip,hashlib,json,os,re,signal,subprocess,sys,time,shutil
from pathlib import Path
from w93_perf import SyncCounters
base=Path(__file__).resolve().parent;tag=sys.argv[1];duration=int(sys.argv[2])
cell=base/tag;cell.mkdir();interval=int(sys.argv[3]);os.environ.update(EFS_RAFT_APPLIED_INTERVAL_MS=str(interval),EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP=str(int(interval==100)),EFS_RAFT_BATCH_NOOP=str(int(interval==100)),EFS_RAFT_GROUP_DRAIN='0')
if len(sys.argv)<5 or sys.argv[4]!='reuse':subprocess.run(['bash',base/'restart.sh','1','1',cell],check=True)
servers={}
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split():
 cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode();n=int(re.search(r'--node-id (\d+)',cmd)[1])
 env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
 flags={k:env.get(k) for k in ('EFS_RAFT_FLOW','EFS_BATCH_OBS','EFS_RAFT_FLOW_EARLY_RETIRE','EFS_RAFT_BATCH_OVERLAP','EFS_RAFT_BATCH_NOOP','EFS_RAFT_GROUP_DRAIN','EFS_COMMANDS_SERVER','EFS_RAFT_APPLIED_INTERVAL_MS')}
 assert flags==dict(EFS_RAFT_FLOW='1',EFS_BATCH_OBS='1',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_BATCH_OVERLAP=str(int(interval==100)),EFS_RAFT_BATCH_NOOP=str(int(interval==100)),EFS_RAFT_GROUP_DRAIN='0',EFS_COMMANDS_SERVER='0',EFS_RAFT_APPLIED_INTERVAL_MS=str(interval)),flags
 servers[n]=dict(pid=int(pid),cmd=cmd,flags=flags,exe_sha256=hashlib.sha256(Path(f'/proc/{pid}/exe').read_bytes()).hexdigest())
assert len(servers)==4
assert len({v["exe_sha256"] for v in servers.values()})==1
version=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True);assert version.startswith('efsd 5b2df4bd ')
(cell/'identity.json').write_text(json.dumps(dict(servers=servers,version=version,sha256=hashlib.sha256(Path('/data1/efs/src/efsd').read_bytes()).hexdigest(),test_harness_commit='current-harness-recorded-in-test-hashes.json'),indent=2))
def require_headroom():
 assert min(shutil.disk_usage(p).free for p in ('/data1/efs/n1','/data2/efs/n3'))>80*2**30,'below 80 GiB free: drain own loader'
 available=int(re.search(r'^MemAvailable:\s+(\d+)',Path('/proc/meminfo').read_text(),re.M)[1])
 assert available>3*2**20,'below 3 GiB available RAM: drain own loader'
require_headroom()
def gc_status():
 return {n:subprocess.check_output(['/data1/efs/src/efs-mgmt','gc-status',f'127.0.0.1:{17431+n}'],text=True,timeout=10) for n in servers}
(cell/'gc-before.json').write_text(json.dumps(gc_status(),indent=2))
clients=subprocess.check_output(['pgrep','-x','efs-fuse'],text=True).split();assert len(clients)==2
logs={n:Path(f'/data1/efs/logs/efsd-n{n}.log') for n in servers};offsets={n:p.stat().st_size for n,p in logs.items()}
root=f'/data1/efs/mnt/tiny-w93-closure-{tag}-20261010'
with (cell/'loader.log').open('w') as out:
 loader=subprocess.Popen(['python3',base/'balanced-load.py',root,cell],stdout=out,stderr=out)
 def drain_owned_loader():
  if loader.poll() is None:
   loader.send_signal(signal.SIGUSR1);loader.wait(timeout=180)
 atexit.register(drain_owned_loader)
 (cell/'loader.pid').write_text(str(loader.pid))
 deadline=time.monotonic()+180
 while not (cell/'ready.json').exists():
  assert loader.poll() is None,'loader failed during balanced preparation'
  assert time.monotonic()<deadline,'balanced preparation exceeded budget';time.sleep(.2)
 time.sleep(7) # preparation mutations settle before baseline counters
 placement=json.loads((cell/'placement.json').read_text());read_samples=[]
 fixed=json.loads(Path('/data1/efs/w93-overlap-hour-20261009/baseline/placement.json').read_text())['probes']
 def read_probe(stage):
  for group,path in fixed.items():
   began=time.monotonic_ns();body=Path(path).read_bytes();elapsed=(time.monotonic_ns()-began)//1000
   assert body==b'W93 immutable read probe'
   read_samples.append(dict(stage=stage,group=int(group),path=path,us=elapsed))
 for i in range(512):read_probe('idle_before')
 time.sleep(6) # exclude idle probe mutations from the timed baseline
 counters=SyncCounters(cell,servers);counters.command('enable')
 (cell/'GO').touch()
 while not (cell/'load-start.json').exists():time.sleep(.01)
 origin=json.loads((cell/'load-start.json').read_text())['mono_us']/1e6
 (cell/'timebase.json').write_text(json.dumps(dict(start_mono_us=int(origin*1e6),duration_seconds=duration)))
 def progress():
  p=cell/'progress.jsonl'
  if not p.exists():return dict(counts=[0]*8,seconds=0)
  lines=p.read_text().splitlines()
  for row in reversed(lines):
   try:return json.loads(row)
   except json.JSONDecodeError:continue
  return dict(counts=[0]*8,seconds=0)
 def statuses():
  result=[]
  for n in servers:
   raw=subprocess.check_output(['/data1/efs/src/efs-mgmt','raft-status',f'127.0.0.1:{17431+n}'],text=True,timeout=10)
   for m in re.finditer(r'group (\d+) hosted=(\d+) role=(\w+) leader=(-?\d+) term=(\d+) commit=(\d+) applied=(\d+)',raw):
    result.append(dict(node=n,group=int(m[1]),hosted=int(m[2]),role=m[3],leader=int(m[4]),term=int(m[5]),commit=int(m[6]),applied=int(m[7])))
  return result
 with (cell/'samples.jsonl').open('w') as f:
  for i in range(duration//5+1):
   due=origin+i*5;time.sleep(max(0,due-time.monotonic()))
   assert loader.poll() is None,'bounded loader ended before timed interval'
   row=dict(seconds=time.monotonic()-origin,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),progress=progress(),diskstats=Path('/proc/diskstats').read_text(),processes={})
   for pid in [v['pid'] for v in servers.values()]+[int(p) for p in clients]+[loader.pid]:row['processes'][pid]={name:Path(f'/proc/{pid}/{name}').read_text() for name in ('stat','io','status')}
   row['raft_status']=statuses()
   if i%12==0:
    require_headroom()
    row['space']=subprocess.check_output(['df','-h','/data1/efs/n1','/data2/efs/n3'],text=True)
    row['gc_status']=gc_status()
   if i%1==0:read_probe('mixed')
   f.write(json.dumps(row)+'\n');f.flush()
 counters.command('disable');counters.stop()
 loader.send_signal(signal.SIGUSR1);rc=loader.wait(timeout=180);assert rc==0,rc
 (cell/'loader-exit.json').write_text(json.dumps(dict(returncode=rc,seconds=time.monotonic()-origin)))
 time.sleep(6)
 for i in range(512):read_probe('idle_after')
 (cell/'read-latency.json').write_text(json.dumps(read_samples,indent=2))
(cell/'gc-after.json').write_text(json.dumps(gc_status(),indent=2))
rows=statuses();cut={r['group']:r['commit'] for r in rows if r['hosted'] and r['role'].lower()=='leader'};assert len(cut)==2,rows
history=[]
for i in range(120):
 rows=statuses();history.append(rows)
 if all(r['applied']>=cut[r['group']] and r['commit']>=cut[r['group']] for r in rows if r['hosted']):break
 time.sleep(.5)
else:raise RuntimeError('replicas did not apply leader cut after drain')
(cell/'catch-up.json').write_text(json.dumps(dict(cut=cut,caught_up=True,observations=history),indent=2))
for n,p in logs.items():
 with p.open('rb') as f:f.seek(offsets[n]);raw=f.read()
 with gzip.open(cell/f'n{n}-window.log.gz','wb') as f:f.write(raw)
 samples=[]
 for line in raw.decode(errors='replace').splitlines():
  if 'batch-obs {' in line:
   try:samples.append(json.loads(line.split('batch-obs ',1)[1]))
   except json.JSONDecodeError:pass
 assert any(r['mono_us']<=origin*1e6 for r in samples),'missing pre-workload diagnostic boundary'
 (cell/f'n{n}-observations.jsonl').write_text('\n'.join(json.dumps(r) for r in samples)+'\n')
(cell/'complete.json').write_text(json.dumps(dict(duration_seconds=duration,tag=tag,applied_interval_ms=interval,root=root,drained=True),indent=2));print(tag,'PASS',flush=True)
