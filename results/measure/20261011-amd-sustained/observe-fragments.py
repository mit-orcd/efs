#!/usr/bin/env python3
"""Compact health sampling; no data-store scans or profiler during timings."""
import datetime,json,subprocess,time
from pathlib import Path
src=Path('/data1/efs/devel-amd-gc-fragments-20261011')
out=Path('/data1/efs/amd-gc-fragments-20261011/live')
end=time.monotonic()+1800
with (out/'status-samples.jsonl').open('a') as log:
 while time.monotonic()<end and not (out/'round.exit').exists():
  row={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'nodes':[]}
  for i in range(4):
   try:
    p=subprocess.run([str(src/'efs-mgmt'),'gc-status',f'127.0.0.1:{24190+i}'],capture_output=True,text=True,timeout=5)
    row['nodes'].append({'node':i+1,'rc':p.returncode,'status':p.stdout+p.stderr})
   except subprocess.TimeoutExpired:row['nodes'].append({'node':i+1,'timeout':True})
  log.write(json.dumps(row)+'\n');log.flush()
  time.sleep(30)
