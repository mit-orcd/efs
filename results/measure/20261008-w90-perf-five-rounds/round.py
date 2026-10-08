#!/usr/bin/env python3
import pathlib,subprocess,json,hashlib,os,sys,re,zipfile,time
root=pathlib.Path('/data1/efs/bench/w90-perf5-20261008');src=root/'src';out=root/sys.argv[1];out.mkdir()
subprocess.run(['cp',str(src/'efs-bench'),str(out/'efs-bench')],check=True)
files=['src/bench/storage_churn.c','src/bench/storage_churn.h','src/bench/latency.h','src/bench/perf_control.h','src/client/efs_bench.c','Makefile']
with zipfile.ZipFile(out/'sources.zip','w',zipfile.ZIP_DEFLATED) as z:
 for f in files:z.write(src/f,f)
manifest={'label':sys.argv[1],'binary_sha256':hashlib.sha256((out/'efs-bench').read_bytes()).hexdigest(),'sources':{f:hashlib.sha256((src/f).read_bytes()).hexdigest() for f in files},'uname':os.uname(),'started':time.time()}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
rows=[]
for label,seconds in [('profile',10),('sample1',20),('sample2',20)]:
 scratch=root/'scratch'/f'{out.name}-{label}';scratch.mkdir()
 cmd=[str(out/'efs-bench'),'--bench','churn','--storage',str(scratch),'--layout','punch','--capacity-bytes',str(8*1024**3),'--container-bytes',str(1024**3),'--payload-size','65536','--occupancy','95','--qd','4','--steps','10000000','--seconds',str(seconds),'--warmup','1000','--read-percent','50','--persist','batch','--batch-size','32','--direct-io']
 env=dict(os.environ,EFS_BENCH_PHASE_MARKERS='1')
 if label=='profile':
  os.mkfifo(out/'control');os.mkfifo(out/'ack')
  env.update(EFS_BENCH_PERF_CONTROL=str(out/'control'),EFS_BENCH_PERF_ACK=str(out/'ack'))
  cmd=['perf','record','--delay=-1','--control',f'fifo:{out}/control,{out}/ack','-e','cpu-clock','-F','199','-g','-o',str(out/'perf.data'),'--',*cmd]
 (out/f'{label}-command.json').write_text(json.dumps(cmd,indent=2))
 with (out/f'{label}-stdout.txt').open('w') as stdout,(out/f'{label}-stderr.txt').open('w') as stderr:
  p=subprocess.run(cmd,env=env,stdout=stdout,stderr=stderr,timeout=600)
 text=(out/f'{label}-stdout.txt').read_text();line=next((s for s in text.splitlines() if s.startswith('BENCH_OK ')),None)
 assert p.returncode==0 and line,(label,p.returncode,(out/f'{label}-stderr.txt').read_text())
 f=dict(re.findall(r'(\w+)=([^\s]+)',line));assert f['errors']=='0' and f['live_objects']==f['verified_objects']
 rows.append(dict(label=label,fields=f));(out/'results.json').write_text(json.dumps(rows,indent=2))
 assert not list(scratch.iterdir());scratch.rmdir();print(out.name,label,f['ops_s'],f['write_p99_us'],flush=True)
 if label=='profile':
  for name,args in [('perf-flat.txt',['--no-children','--sort','dso,symbol']),('perf-callers.txt',['--children','--sort','symbol'])]:
   with (out/name).open('w') as f:subprocess.run(['perf','report','-i',str(out/'perf.data'),'--stdio','--percent-limit','0.5',*args],stdout=f,stderr=subprocess.STDOUT,check=True)
print('ROUND_PASS',out.name,flush=True)
