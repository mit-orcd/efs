#!/usr/bin/env python3
"""Matched nuc steady-state study. Stages execute serially; reports run last."""
import sys,pathlib,tempfile,json,random,shutil,itertools,os,hashlib,time,subprocess
sys.path.insert(0,'/tmp/efs-nuc-sync-study-20261007/scripts')
import bench_profile as b
out=pathlib.Path('/tmp/efs-nuc-latency-20261007');out.mkdir(exist_ok=False)
roots=['/data1/efs/bench','/data2/efs/bench']
bins={}
for variant in ['current','conditional']:
 p=out/'binaries'/variant/'efs-bench';p.parent.mkdir(parents=True)
 shutil.copy2(f'/tmp/efs-nuc-latency-{variant}-20261007',p);bins[variant]=str(p)
manifest=dict(start=time.time(),seed=30043,phase_order=['baseline','perf','strace','reports'],baseline_seconds=30,repeats=5,qd=16,window_per_worker=64,fragment_bytes=65536,roots=roots,binaries={k:dict(path=v,sha256=hashlib.sha256(pathlib.Path(v).read_bytes()).hexdigest()) for k,v in bins.items()})
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
for name,cmd in [('uname',['uname','-a']),('cpu',['lscpu']),('devices',['lsblk','-o','NAME,MODEL,SIZE,TYPE,FSTYPE,MOUNTPOINTS']),('processes',['ps','-eo','pid,comm,args']),('memory',['free','-h'])]:b.execute(cmd,out,name,30)
for i,r in enumerate(roots):b.execute(['findmnt','-T',r],out,f'mount-root{i+1}',30)
rows=[]
configs=list(itertools.product(bins,range(2),[False,True]))
work=list(itertools.product(range(5),configs));random.Random(30043).shuffle(work)
for phase,items,seconds in [('baseline',work,30),('perf',[(0,c) for c in configs],30),('strace',[(0,c) for c in configs],5)]:
 print('PHASE',phase,flush=True)
 for n,(repeat,(variant,rootidx,sync)) in enumerate(items,1):
  name=f'{variant}-root{rootidx+1}-sync{int(sync)}-r{repeat}'
  directory=out/phase/name;directory.mkdir(parents=True)
  scratch=tempfile.mkdtemp(prefix='efs-latency-',dir=roots[rootidx])
  env=os.environ.copy()
  for key in ['EFS_PERF_PATH','EFS_BENCH_PERF_CONTROL','EFS_BENCH_PERF_ACK']:env.pop(key,None)
  cmd=[bins[variant],'--bench','data','--rw','write','--write-mode','overwrite','--qd','16','--window','64','--time',str(seconds),'--direct-io','--full-paths','--skip-ceiling','--storage',scratch]+(['--sync'] if sync else [])
  command=cmd[:]
  if phase=='perf':
   control=str(directory/'control');ack=str(directory/'ack');os.mkfifo(control);os.mkfifo(ack)
   env.update(EFS_BENCH_PERF_CONTROL=control,EFS_BENCH_PERF_ACK=ack)
   cmd=['perf','record','-e','cycles','-F','199','-g','--call-graph','dwarf','-o',str(directory/'perf.data'),'--delay=-1','--control',f'fifo:{control},{ack}','--']+cmd
  elif phase=='strace':
   env['EFS_BENCH_PHASE_MARKERS']='1'
   cmd=['strace','-f','-ttt','-T','-s','128','-e','trace=openat,write,writev,newfstatat,fstat,ftruncate,fsync,fdatasync,close,futex','-o',str(directory/'trace.txt')]+cmd
  try:
   row=dict(name=name,phase=phase,variant=variant,root=rootidx+1,sync=sync,repeat=repeat,command=command,load_start=os.getloadavg(),start=time.time())
   row['run']=b.execute(cmd,directory,phase,180,env);row['end']=time.time();row['load_end']=os.getloadavg();row['metrics']=b.metric_rows(directory/f'{phase}.stdout')
   row['valid']=row['run']['returncode']==0 and b.valid_metrics(row['metrics'])
   rows.append(row);(out/'results.json').write_text(json.dumps(rows,indent=2))
   m=row['metrics'][0] if row['metrics'] else {}
   print(f'{phase} {n}/{len(items)} {name} valid={row["valid"]} GiB_s={m.get("GiB_s")} p99_us={m.get("p99_us")} samples={m.get("lat_samples")}',flush=True)
   if not row['valid']:raise RuntimeError('invalid workload '+name)
  finally:
   shutil.rmtree(scratch)
   if phase=='perf':pathlib.Path(control).unlink();pathlib.Path(ack).unlink()
 print('FINISHED',phase,flush=True)
# All workloads have finished before report generation.
a=b.parser().parse_args(['--timeout','180'])
for row in rows:
 if row['phase']=='perf':
  directory=out/'perf'/row['name'];good,symbols=b.reports('perf',directory,a)
  row['reports_valid']=bool(good);row['symbols']=symbols
  (out/'results.json').write_text(json.dumps(rows,indent=2))
  print('REPORTS',row['name'],good,flush=True)
print('DONE',flush=True)
