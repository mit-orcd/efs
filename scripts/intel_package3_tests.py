#!/usr/bin/env python3
"""Intel ordinary-files package 3 gates. Run native first; preserve raw output outside Git.

Mounted core/extras use existing budgets, isolated unique tags, and 16 jobs.
Two real opposite-host mounts run concurrently. No feature/deadline changes.
This driver does not restart services or remove any pre-existing payloads.
"""
import argparse,concurrent.futures,json,shlex,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('gate',choices=['core','extras','runtime','latency','concurrent']);p.add_argument('--output',type=Path,required=True);p.add_argument('--run-id',required=True);p.add_argument('--seconds',type=int,default=600);p.add_argument('--phase-observer',action='store_true',help='diagnostic only: preserve capabilities and record deep calls; not acceptance');a=p.parse_args()
assert not a.phase_observer or a.gate=='concurrent'
assert a.run_id and all(c.isalnum() or c in '-_' for c in a.run_id)
a.output.mkdir(parents=True,exist_ok=True)
def run(host):
 name=a.gate+'-'+host+'-'+a.run_id;root='/data1/efs/package3-results/'+a.run_id;src='/data1/efs/src'
 subprocess.run(['ssh',host,shlex.join(['mkdir','-p',root])],check=True)
 if a.gate=='concurrent':
  if a.phase_observer:
   observer=root+'/observer';subprocess.run(['ssh',host,shlex.join(['mkdir','-p',observer])],check=True)
   subprocess.run(['scp',str(Path(__file__).with_name('container_deepsteps_sitecustomize.py')),host+':'+observer+'/sitecustomize.py'],check=True)
   subprocess.run(['scp',str(Path(__file__).with_name('posix_phase_runner.py')),host+':'+root+'/phase-runner.py'],check=True)
  mounts=['/data1/efs/mnt'];created=[]
  seed='10.0.0.209:17432' if host=='intel1_efs' else '10.0.0.207:17432'
  try:
   for client in range(1,4 if host=='intel1_efs' else 3):
    mount='/data1/efs/package3-mnt/'+a.run_id+'-'+str(client)
    cmd='EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_PACKED_WRITES=0 EFS_EXPIRY=0 '+shlex.join([src+'/scripts/client.sh',seed,mount])
    created.append(mount);subprocess.run(['ssh',host,cmd],check=True,timeout=75);mounts.append(mount)
   def worker(pair):
    client,mount=pair;tag=name+'-'+str(client);args=['python3',src+'/tests/posix/posix_suite.py',mount,'--suite','core','--jobs','16','--timeout-s','30','--tag',tag,'--results',root+'/'+tag+'.tsv'];started=time.monotonic()
    if a.phase_observer:args=['python3',root+'/phase-runner.py',root+'/'+tag+'-phases.log',observer]+args[1:]
    with (a.output/(tag+'.log')).open('w') as f:r=subprocess.run(['ssh',host,shlex.join(['timeout','--kill-after=10s','900s']+args)],stdout=f,stderr=subprocess.STDOUT,timeout=930)
    subprocess.run(['scp',host+':'+root+'/'+tag+'.tsv',str(a.output/(tag+'.tsv'))],check=True)
    if a.phase_observer:subprocess.run(['scp',host+':'+root+'/'+tag+'-phases.log',str(a.output/(tag+'-phases.log'))],check=True)
    row={'observer':a.phase_observer,'host':host,'client':client,'gate':a.gate,'rc':r.returncode,'seconds':round(time.monotonic()-started,3),'args':args};print(json.dumps(row),flush=True);return row
   with concurrent.futures.ThreadPoolExecutor(len(mounts)) as pool:results=list(pool.map(worker,enumerate(mounts)))
   return {'host':host,'gate':a.gate,'rc':int(any(x['rc'] for x in results)),'clients':results}
  finally:
   for mount in created:subprocess.run(['ssh',host,shlex.join([src+'/scripts/client.sh','stop',mount])],check=True,timeout=90)
 if a.gate=='runtime':args=['python3','/data1/efs/devops/intel_package3_verify.py'];timeout=90
 elif a.gate=='latency':args=['python3',src+'/tests/live/efs_foreground_latency.py','/data1/efs/mnt',root+'/'+name,'--seconds',str(a.seconds),'--period','1'];timeout=a.seconds+120
 else:args=['python3',src+'/tests/posix/posix_suite.py','/data1/efs/mnt','--suite',a.gate,'--jobs','16','--timeout-s','30','--tag',name,'--results',root+'/'+name+'.tsv'];timeout=900
 cmd=shlex.join(['timeout','--signal=TERM','--kill-after=10s',str(timeout)+'s']+args)
 started=time.monotonic()
 with (a.output/(name+'.log')).open('w') as f:r=subprocess.run(['ssh',host,cmd],stdout=f,stderr=subprocess.STDOUT,timeout=timeout+30)
 if a.gate in ('core','extras'):subprocess.run(['scp',host+':'+root+'/'+name+'.tsv',str(a.output/(name+'.tsv'))],check=True)
 row={'host':host,'gate':a.gate,'rc':r.returncode,'seconds':round(time.monotonic()-started,3),'args':args}
 print(json.dumps(row),flush=True);return row
with concurrent.futures.ThreadPoolExecutor(2) as pool:rows=list(pool.map(run,['intel1_efs','intel2_efs']))
(a.output/(a.gate+'-summary.json')).write_text(json.dumps(rows,indent=2)+'\n');raise SystemExit(any(r['rc'] for r in rows))
