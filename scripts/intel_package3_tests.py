#!/usr/bin/env python3
"""Intel ordinary-files package 3 gates. Run native first; preserve raw output outside Git.

Mounted core/extras use existing budgets, isolated unique tags, and 16 jobs.
Two real opposite-host mounts run concurrently. No feature/deadline changes.
This driver does not restart services or remove any pre-existing payloads.
"""
import argparse,concurrent.futures,json,shlex,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('gate',choices=['core','extras','runtime','latency']);p.add_argument('--output',type=Path,required=True);p.add_argument('--run-id',required=True);p.add_argument('--seconds',type=int,default=600);a=p.parse_args()
assert a.run_id and all(c.isalnum() or c in '-_' for c in a.run_id)
a.output.mkdir(parents=True,exist_ok=True)
def run(host):
 name=a.gate+'-'+host+'-'+a.run_id;root='/data1/efs/package3-results/'+a.run_id;src='/data1/efs/src'
 subprocess.run(['ssh',host,shlex.join(['mkdir','-p',root])],check=True)
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
