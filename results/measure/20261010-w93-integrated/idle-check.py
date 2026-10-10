"""More-powered matched read-only probe; separate checkpoint and cohort switches."""
import hashlib,json,os,re,subprocess,sys,time
from pathlib import Path
base=Path(__file__).resolve().parent;tag=sys.argv[1];interval=int(sys.argv[2]);cohort=int(sys.argv[3]);count=int(sys.argv[4])
cell=base/tag;cell.mkdir()
os.environ.update(EFS_RAFT_APPLIED_INTERVAL_MS=str(interval),EFS_RAFT_BATCH_NOOP=str(cohort),EFS_RAFT_BATCH_OVERLAP=str(cohort),EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_FLOW_EARLY_RETIRE='0')
if len(sys.argv)<6 or sys.argv[5]!='reuse':subprocess.run(['bash',base/'restart.sh','1','0',cell],check=True)
identity=[]
for pid in subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split():
 env=dict(x.split('=',1) for x in Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in x)
 for k,v in dict(EFS_RAFT_APPLIED_INTERVAL_MS=str(interval),EFS_RAFT_BATCH_NOOP=str(cohort),EFS_RAFT_BATCH_OVERLAP=str(cohort),EFS_BATCH_OBS='0',EFS_COMMANDS_SERVER='0').items():assert env.get(k)==v,(pid,k,env.get(k))
 identity.append(dict(pid=int(pid),env={k:env[k] for k in env if k.startswith('EFS_')},sha256=hashlib.sha256(Path(f'/proc/{pid}/exe').read_bytes()).hexdigest()))
assert len(identity)==4 and len({r['sha256'] for r in identity})==1
probes=json.loads(Path('/data1/efs/w93-overlap-hour-20261009/baseline/placement.json').read_text())['probes'];samples={g:[] for g in probes}
time.sleep(8)
for i in range(512):
 for g in probes:assert Path(probes[g]).read_bytes()==b'W93 immutable read probe'
time.sleep(6)
began=time.monotonic()
with (cell/'reads.jsonl').open('w') as out:
 for i in range(count):
  # Keep the paired order identical to the sustained workload probes.
  for g in probes:
   start=time.monotonic_ns();body=Path(probes[g]).read_bytes();us=(time.monotonic_ns()-start)//1000
   assert body==b'W93 immutable read probe';samples[g].append(us)
   out.write(json.dumps(dict(iteration=i,group=int(g),us=us))+'\n')
  if i%128==0:out.flush()
summary={}
for g,v in samples.items():
 a=sorted(v);summary[g]=dict(n=len(a),mean_us=sum(a)/len(a),**{f'p{q}_us':a[(len(a)*q+99)//100-1] for q in (50,95,99)},max_us=a[-1])
roles={n:subprocess.check_output(['/data1/efs/src/efs-mgmt','raft-status',f'127.0.0.1:{17431+n}'],text=True,timeout=10) for n in range(1,5)}
result=dict(tag=tag,applied_interval_ms=interval,cohort=cohort,seconds=time.monotonic()-began,identity=identity,reads=summary,raft_status=roles)
(cell/'complete.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:v for k,v in result.items() if k not in ('identity','raft_status')}),flush=True)
