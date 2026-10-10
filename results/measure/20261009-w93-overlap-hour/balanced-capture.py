"""Same frozen binary; 900-second balanced metadata-group comparison cell."""
import gzip,hashlib,json,os,re,signal,subprocess,sys,time
from pathlib import Path
base=Path(__file__).resolve().parent;tag=sys.argv[1];duration=int(sys.argv[2])
cell=base/tag;cell.mkdir();ov=int(sys.argv[3]);os.environ.update(EFS_RAFT_BATCH_OVERLAP=str(ov),EFS_RAFT_BATCH_NOOP='1',EFS_RAFT_GROUP_DRAIN='0')
subprocess.run(['bash',base/'restart.sh','1','1',cell],check=True)
servers={}
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split():
 cmd=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode();n=int(re.search(r'--node-id (\d+)',cmd)[1])
 env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
 flags={k:env.get(k) for k in ('EFS_RAFT_FLOW','EFS_BATCH_OBS','EFS_RAFT_BATCH_OVERLAP','EFS_RAFT_BATCH_NOOP','EFS_RAFT_GROUP_DRAIN','EFS_COMMANDS_SERVER')}
 assert flags==dict(EFS_RAFT_FLOW='1',EFS_BATCH_OBS='1',EFS_RAFT_BATCH_OVERLAP=str(ov),EFS_RAFT_BATCH_NOOP='1',EFS_RAFT_GROUP_DRAIN='0',EFS_COMMANDS_SERVER='0'),flags
 servers[n]=dict(pid=int(pid),cmd=cmd,flags=flags)
assert len(servers)==4
version=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True);assert version.startswith('efsd ca810bac ')
(cell/'identity.json').write_text(json.dumps(dict(servers=servers,version=version,sha256=hashlib.sha256(Path('/data1/efs/src/efsd').read_bytes()).hexdigest()),indent=2))
logs={n:Path(f'/data1/efs/logs/efsd-n{n}.log') for n in servers};offsets={n:p.stat().st_size for n,p in logs.items()}
root=f'/data1/efs/mnt/tiny-w93-overlap-balanced-{tag}-20261009'
with (cell/'loader.log').open('w') as out:
 loader=subprocess.Popen(['python3',base/'balanced-load.py',root,cell],stdout=out,stderr=out)
 (cell/'loader.pid').write_text(str(loader.pid))
 deadline=time.monotonic()+180
 while not (cell/'ready.json').exists():
  assert loader.poll() is None,'loader failed during balanced preparation'
  assert time.monotonic()<deadline,'balanced preparation exceeded budget';time.sleep(.2)
 time.sleep(7) # preparation mutations settle before baseline counters
 placement=json.loads((cell/'placement.json').read_text());read_samples=[]
 def read_probe(stage):
  for group,path in placement['probes'].items():
   began=time.monotonic_ns();body=Path(path).read_bytes();elapsed=(time.monotonic_ns()-began)//1000
   assert body==b'W93 immutable read probe'
   read_samples.append(dict(stage=stage,group=int(group),us=elapsed))
 for i in range(8):read_probe('idle_before')
 time.sleep(6) # exclude idle probe mutations from the timed baseline
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
   for pid in [v['pid'] for v in servers.values()]+[loader.pid]:row['processes'][pid]={name:Path(f'/proc/{pid}/{name}').read_text() for name in ('stat','io','status')}
   if i%60==0:row['raft_status']=statuses()
   if i%12==0:row['space']=subprocess.check_output(['df','-h','/data1/efs/n1','/data2/efs/n3'],text=True)
   if i%12==0:read_probe('mixed')
   f.write(json.dumps(row)+'\n');f.flush()
 loader.send_signal(signal.SIGUSR1);rc=loader.wait(timeout=180);assert rc==0,rc
 (cell/'loader-exit.json').write_text(json.dumps(dict(returncode=rc,seconds=time.monotonic()-origin)))
 time.sleep(6)
 for i in range(8):read_probe('idle_after')
 (cell/'read-latency.json').write_text(json.dumps(read_samples,indent=2))
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
(cell/'complete.json').write_text(json.dumps(dict(duration_seconds=duration,tag=tag,overlap=ov,root=root,drained=True),indent=2));print(tag,'PASS',flush=True)
