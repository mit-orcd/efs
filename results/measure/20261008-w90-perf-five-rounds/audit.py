#!/usr/bin/env python3
import pathlib,subprocess,json,os,re,collections,sys,gzip
root=pathlib.Path('/data1/efs/bench/w90-perf5-20261008')
for version in ['baseline','round5-final']:
 out=root/f'strace-{version}'
 if (out/'summary.json').exists():print('STRACE_PASS',version,'previously verified',flush=True);continue
 out.mkdir(exist_ok=True);scratch=root/'scratch'/out.name;scratch.mkdir(exist_ok=True)
 cmd=[str(root/version/'efs-bench'),'--bench','churn','--storage',str(scratch),'--layout','punch','--capacity-bytes',str(8*1024**3),'--container-bytes',str(1024**3),'--payload-size','65536','--occupancy','95','--qd','4','--steps','10000000','--seconds','5','--warmup','1000','--read-percent','50','--persist','batch','--batch-size','32','--direct-io']
 trace=out/'trace.txt';full=['strace','-f','-ttt','-T','-yy','-s','64','-e','trace=pread64,pwrite64,fallocate,fsync,fstat,newfstatat,statx,openat,close,ioctl,write','-o',str(trace),*cmd]
 (out/'command.json').write_text(json.dumps(full,indent=2))
 if not trace.exists():
  with (out/'stdout.txt').open('w') as stdout,(out/'stderr.txt').open('w') as stderr:
   p=subprocess.run(full,env=dict(os.environ,EFS_BENCH_PHASE_MARKERS='1'),stdout=stdout,stderr=stderr,timeout=600)
  assert p.returncode==0,(out,p.returncode)
 fields=dict(re.findall(r'(\w+)=([^\s]+)',next(s for s in (out/'stdout.txt').read_text().splitlines() if s.startswith('BENCH_OK '))))
 assert fields['errors']=='0' and fields['live_objects']==fields['verified_objects']
 begin=end=None;pending={};calls=[]
 for line in trace.open():
  m=re.match(r'\s*(\d+)\s+(\d+\.\d+)\s+(.*)',line)
  if not m:continue
  pid,stamp,body=m.groups();stamp=float(stamp)
  if 'BENCH_PHASE begin' in body:begin=stamp
  if 'BENCH_PHASE end' in body:end=stamp
  if '<unfinished ...>' in body:
   pending[pid]=(stamp,body.replace('<unfinished ...>',''));continue
  if body.startswith('<... '):
   old=pending.pop(pid,None)
   if not old:continue
   stamp,first=old;body=first+body.split(' resumed>',1)[1]
  m=re.match(r'(\w+)\(.*\)\s+=\s+(-?\d+).*?(?:<(\d+\.\d+)>)?$',body)
  if m:
   name,ret,duration=m.groups();dur=re.search(r'<([0-9]+\.[0-9]+)>$',body)
   calls.append((stamp,name,int(ret),float(dur.group(1)) if dur else 0,body))
 assert begin and end and end>begin
 counts=collections.Counter();times=collections.Counter();errors=[];directory_syncs=0;io_sizes=collections.Counter();punch_bytes=0
 with (out/'measured-calls.txt').open('w') as keep:
  for stamp,name,ret,secs,body in calls:
   if not begin<=stamp<end:continue
   counts[name]+=1;times[name]+=secs;keep.write(f'{stamp:.6f} {body}\n')
   if ret<0:errors.append(body)
   if name=='fsync' and re.search(r'/churn-\d+>\s*\)',body):directory_syncs+=1
   if name in ['pread64','pwrite64']:io_sizes[ret]+=1
   if name=='fallocate':
    size=re.search(r', (\d+)\s*\)\s+=',body);assert size,body;punch_bytes+=int(size.group(1))
 assert not errors,errors[:3]
 assert counts['pread64']==int(fields['reads']),(counts,fields)
 assert counts['pwrite64']==int(fields['writes']),(counts,fields)
 assert counts['fsync']==int(fields['syncs']),(counts,fields)
 assert punch_bytes==int(fields['retired'])*69632,(punch_bytes,fields)
 assert set(io_sizes)=={69632},io_sizes
 assert counts['openat']==counts['close']==counts['ioctl']==0,counts
 if version=='round5-final':assert directory_syncs==0,directory_syncs
 summary=dict(fields=fields,phase_seconds=end-begin,counts=counts,syscall_seconds=times,directory_syncs=directory_syncs,io_return_sizes=io_sizes,punched_bytes=punch_bytes,errors=errors)
 (out/'summary.json').write_text(json.dumps(summary,indent=2))
 with trace.open('rb') as src,gzip.open(out/'trace.txt.gz','wb') as dst:
  import shutil;shutil.copyfileobj(src,dst)
 trace.unlink();assert not list(scratch.iterdir());scratch.rmdir();print('STRACE_PASS',version,json.dumps(counts),flush=True)
