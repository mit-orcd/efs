#!/usr/bin/env python3
"""Run bounded container-only Intel gates on a previously initialized private cluster.

Build/deploy with intel_container_node.py first. No installed service is stopped.
Outputs stay below the supplied local artifact directory, outside Git.
"""
import argparse,concurrent.futures,json,shlex,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('gate',choices=['core','peer','mmap','concurrent','lifecycle-create','lifecycle-cold','lifecycle-delete']);p.add_argument('--base',default='/data1/efs/bench/container-3173be61-20261012');p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
s=a.base+'/src';remoteout=a.base+'/artifacts';rows=[]
def run(host,index,name,argv,timeout):
 c={'name':name,'argv':argv,'timeout':timeout};cmd=['python3',a.base+'/intel_container_node.py','run','--base',a.base,'--host-index',str(index),'--command',json.dumps(c)]
 start=time.monotonic()
 with (a.output/(name+'.log')).open('w') as f:
  try:r=subprocess.run(['ssh',host,shlex.join(cmd)],stdout=f,stderr=subprocess.STDOUT,timeout=timeout+30);rc=r.returncode
  except subprocess.TimeoutExpired:rc=124
 row={'name':name,'host':host,'argv':argv,'returncode':rc,'seconds':round(time.monotonic()-start,3),'status':'PASS' if rc==0 else 'FAIL'};print(json.dumps(row),flush=True);return row
jobs=[]
for index,host in enumerate(['intel1_efs','intel2_efs']):
 m=a.base+'/mnt0';name=a.gate+'-intel'+str(index+1)
 if a.gate in ('core','mmap','concurrent'):
  for client in range((4 if index==0 else 3) if a.gate=='concurrent' else 1):
   tag=name+'-'+str(client);argv=['python3',s+'/tests/posix/posix_suite.py',a.base+'/mnt'+str(client),'--suite','extras' if a.gate=='mmap' else 'core','--jobs','16','--timeout-s','30','--tag',tag,'--results',remoteout+'/'+tag+'.tsv']
   if a.gate=='mmap':argv+=['--filter','mmap']
   jobs.append((host,index,tag,argv,600))
 elif a.gate=='peer':jobs.append((host,index,name,['python3',s+'/tests/posix/posix_2client.py','--local',m,a.base+'/mnt1','--parent',name,'--results',remoteout+'/'+name+'.tsv'],600))
 elif index==0:
  phase={'lifecycle-create':'create','lifecycle-cold':'verify','lifecycle-delete':'delete'}[a.gate];jobs.append((host,index,name,['python3',s+'/tests/live/storage_engine_ab.py',phase,'--mount',m,'--state',remoteout+'/lifecycle-state.json','--output',remoteout+'/'+name+'.json','--seconds','0'],600))
with concurrent.futures.ThreadPoolExecutor(max_workers=len(jobs)) as pool:rows=list(pool.map(lambda j:run(*j),jobs))
(a.output/(a.gate+'-summary.json')).write_text(json.dumps(rows,indent=2));raise SystemExit(1 if any(r['returncode'] for r in rows) else 0)
