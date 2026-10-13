#!/usr/bin/env python3
"""Normal-budget Intel snapshot-pressure/foreground latency and resource evidence.

Run after other suites finish. No profiler flags or threshold changes. Collection
of bounded daemon tails happens AFTER timed workloads, never during them.
"""
import argparse,concurrent.futures,json,shlex,subprocess,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--run-id',required=True);p.add_argument('--seconds',type=int,default=600);a=p.parse_args();assert a.run_id and all(c.isalnum() or c in '-_' for c in a.run_id);assert a.seconds>0
a.output.mkdir(parents=True,exist_ok=True);src='/data1/efs/src';remote='/data1/efs/package3-results/'+a.run_id
hosts=['intel1_efs','intel2_efs'];offsets={}
for h in hosts:
 subprocess.run(['ssh',h,shlex.join(['mkdir','-p',remote])],check=True)
 code="import pathlib,json;print(json.dumps({str(p):p.stat().st_size for p in pathlib.Path('/data1/efs/logs').glob('efsd-n*.log')}))"
 offsets[h]=json.loads(subprocess.check_output(['ssh',h,'python3 -c '+shlex.quote(code)],text=True))
(a.output/'start-offsets.json').write_text(json.dumps(offsets,indent=2))
def run(job):
 h,name,args=job;started=time.monotonic()
 with (a.output/(h+'-'+name+'.log')).open('w') as f:r=subprocess.run(['ssh',h,shlex.join(['timeout','--kill-after=10s',str(a.seconds+180)+'s']+args)],stdout=f,stderr=subprocess.STDOUT,timeout=a.seconds+200)
 row={'host':h,'name':name,'rc':r.returncode,'seconds':round(time.monotonic()-started,3),'args':args};print(json.dumps(row),flush=True)
 subprocess.run(['scp',h+':'+remote+'/'+name+'/complete.json',str(a.output/(h+'-'+name+'-complete.json'))],check=False)
 return row
jobs=[]
for index,h in enumerate(hosts):
 jobs+=[(h,'foreground',['python3',src+'/tests/live/efs_foreground_latency.py','/data1/efs/mnt',remote+'/foreground','--seconds',str(a.seconds),'--period','1']), (h,'snapshot',['python3',src+'/tests/live/efs_snapshot_pressure.py','/data1/efs/mnt',remote+'/snapshot','--group',str(index*2),'--seconds',str(a.seconds)])]
code="import pathlib,subprocess,time,sys;end=time.monotonic()+int(sys.argv[1]);\nwhile time.monotonic()<end:\n subprocess.run(['python3','/data1/efs/src/tests/live/efs_process_resources.py'],check=True);time.sleep(10)"
with concurrent.futures.ThreadPoolExecutor(6) as pool:
 work=[pool.submit(run,j) for j in jobs]
 resources=[]
 for h in hosts:
  f=(a.output/(h+'-resources.jsonl')).open('w');proc=subprocess.Popen(['ssh',h,'python3 -c '+shlex.quote(code)+' '+str(a.seconds)],stdout=f,stderr=subprocess.STDOUT);resources.append((h,proc,f))
 rows=[f.result() for f in work]
 for h,proc,f in resources:rc=proc.wait(timeout=30);f.close();assert rc==0,(h,rc)
(a.output/'summary.json').write_text(json.dumps(rows,indent=2))
for h in hosts:
 for path,offset in offsets[h].items():
  code="import pathlib,sys;path=pathlib.Path(sys.argv[1]);offset=int(sys.argv[2]);size=path.stat().st_size;assert size>=offset, 'daemon log rotated during trial';f=path.open('rb');f.seek(max(offset,size-12*1024*1024));sys.stdout.buffer.write(f.read(12*1024*1024))"
  with (a.output/(h+'-'+Path(path).name+'.tail')).open('wb') as f:subprocess.run(['ssh',h,shlex.join(['python3','-c',code,path,str(offset)])],stdout=f,check=True)
raise SystemExit(any(r['rc'] for r in rows))
