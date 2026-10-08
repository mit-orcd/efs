import subprocess,os,json,time,hashlib
from pathlib import Path
out=Path('/private/tmp/efs-v020-matrix');out.mkdir(exist_ok=True)
configs=[('direct','rdma'),('direct','tcp'),('buffered','rdma'),('buffered','tcp')]
results=[]
for mode,transport in configs:
 label=f'{mode}-{transport}';log=open(out/f'{label}.log','w');start=time.monotonic()
 env=f'EFS_CLUSTER=amd EFS_DIRECT_IO={int(mode=="direct")} EFS_TRANSPORT={transport} EFS_RDMA_DEV=rxe0 EFS_RDMA_GID_INDEX=1'
 base='/data1/efs/devops/'
 print('BEGIN '+label,flush=True)
 for step in ['stop','start','smoke','smoke2','smoke-persist','status']:
  cmd=env+' '+base+step+'.sh'
  r=subprocess.run(['ssh','amd_efs',cmd],stdout=log,stderr=subprocess.STDOUT,timeout=360)
  log.flush()
  if r.returncode:raise RuntimeError((label,step,r.returncode))
 # Verify actual restarted daemon args and both mounts' transport selection.
 check=r'''python3 - <<'REMOTE'
import json,os
from pathlib import Path
rows=[]
for p in Path('/proc').glob('[0-9]*'):
 try:
  args=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode()
  exe=os.readlink(p/'exe')
  if exe not in ('/data1/efs/src/efsd','/data1/efs/src/efs-fuse'):continue
  env=dict(x.split('=',1) for x in (p/'environ').read_bytes().decode().split('\0') if '=' in x)
  verbs=[]
  for fd in (p/'fd').iterdir():
   try:
    target=os.readlink(fd)
    if 'uverbs' in target:verbs.append(target)
   except OSError:pass
  rows.append({'pid':int(p.name),'args':args,'transport':env.get('EFS_TRANSPORT'),'verbs':verbs})
 except (PermissionError,FileNotFoundError,ProcessLookupError):pass
print(json.dumps(rows,indent=2))
REMOTE'''
 live=subprocess.check_output(['ssh','amd_efs',check],text=True);(out/f'{label}-processes.json').write_text(live);rows=json.loads(live)
 assert len(rows)==6,(label,rows)
 for row in rows:
  assert row['transport']==transport,row
  if '/efsd ' in row['args']:assert ('--direct-io ' in row['args'])==(mode=='direct'),row
  assert bool(row['verbs'])==(transport=='rdma'),row
 log.close();results.append({'mode':mode,'transport':transport,'exitcode':0,'seconds':time.monotonic()-start,'six_processes_verified':True})
 (out/'matrix.json').write_text(json.dumps(results,indent=2));print('PASS '+label,flush=True)
