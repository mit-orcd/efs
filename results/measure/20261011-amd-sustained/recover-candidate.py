#!/usr/bin/env python3
"""Observe cold GC on only the retained, failed private AMD fixture."""
import json,os
from pathlib import Path
import signal,subprocess,time
root=Path('/data1/efs/amd-gc-candidate-20261011/live')
src=Path('/data1/efs/devel-amd-gc-candidate-20261011')
manifest=next(p for p in root.glob('gc-direct-*/manifest.json') if (p.parent/'tiny-sustained.json').exists())
roots=[Path(p) for p in json.loads(manifest.read_text())['roots']]
assert all(p.is_dir() and p.is_relative_to(root) for p in roots)
servers=[];handles=[];samples=[]
env=os.environ|{'EFS_TRANSPORT':'tcp','EFS_COMMANDS':'0','EFS_COMMANDS_SERVER':'0','EFS_GC_DBG':'1','EFS_RAFT_OBS':'1','EFS_GC_BACKLOG_IDLE_MS':'100'}
try:
 for i,p in enumerate(roots):
  handle=(root/f'recovery-node-{i+1}.log').open('wb');handles.append(handle)
  cmd=[str(src/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(24190+i),'--storage',str(p),'--quota','4G','--direct-io']
  if i:cmd+=['--join','127.0.0.1:24190']
  servers.append(subprocess.Popen(cmd,stdout=handle,stderr=handle,env=env))
  time.sleep(.3)
 for elapsed in range(0,61,10):
  if elapsed:time.sleep(10)
  row={'seconds':elapsed,'nodes':[]}
  for i,p in enumerate(roots):
   result=subprocess.run([str(src/'efs-mgmt'),'gc-status',f'127.0.0.1:{24190+i}'],capture_output=True,text=True,timeout=10)
   inventory=[f for f in (p/'data/exports/1').glob('**/*') if f.is_file()]
   blocks=[];raced=0
   for f in inventory:
    try:blocks.append(f.stat().st_blocks*512)
    except FileNotFoundError:raced+=1
   row['nodes'].append({'status':result.stdout+result.stderr,'status_rc':result.returncode,'files':len(blocks),'allocated_bytes':sum(blocks),'concurrent_disappearances':raced})
  samples.append(row);(root/'cold-gc-recovery.json').write_text(json.dumps(samples,indent=2)+'\n')
  print('recovery',elapsed,[(n['files'],n['allocated_bytes']) for n in row['nodes']],flush=True)
finally:
 for p in servers:
  if p.poll() is None:p.send_signal(signal.SIGTERM)
 for p in servers:
  try:p.wait(timeout=30)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 for h in handles:h.close()
