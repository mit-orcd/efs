#!/usr/bin/env python3
import json,subprocess,time
from pathlib import Path
out=Path('/data1/efs/amd-overload-20261011')
mgmt='/data1/efs/devel-amd-overload-20261011/efs-mgmt'
start=time.monotonic()
while not (out/'queue.exit').exists(): time.sleep(15)
with (out/'long-status-samples.jsonl').open('a') as f:
 while time.monotonic()-start<3600 and not (out/'long-queue.exit').exists():
  nodes=[]
  for port in range(24210,24214):
   try:
    r=subprocess.run([mgmt,'gc-status',f'127.0.0.1:{port}'],capture_output=True,text=True,timeout=3)
    nodes.append({'port':port,'rc':r.returncode,'status':r.stdout[-5000:]})
   except subprocess.TimeoutExpired: nodes.append({'port':port,'timeout':True})
  f.write(json.dumps({'epoch':time.time(),'nodes':nodes})+'\n');f.flush()
  time.sleep(30)
