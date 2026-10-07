import sys,pathlib,json,tempfile,subprocess,os,time,statistics,random
sys.path.insert(0,'/tmp/efs-writer-study-20261007/scripts');import bench_profile as b
out=pathlib.Path('/tmp/efs-writer-study-device-20261007');out.mkdir(exist_ok=False)
binary='/tmp/efs-writer-study-results-final-20261007/efs-bench'
roots=['/data1/efs/bench','/home/efs/additional-work-dir/bench'];rows=[]
checks=[(i,r,shape) for r in range(3) for i in range(2) for shape in ['append_fsync','overwrite_fdatasync','create_rename_dirsync']];random.Random(3807).shuffle(checks)
for i,r,shape in checks:
 with tempfile.TemporaryDirectory(prefix='efs-device-study-',dir=roots[i]) as scratch:
  fd=os.open(scratch+'/body',os.O_CREAT|os.O_RDWR,0o600);os.write(fd,b'x'*4096);os.fsync(fd)
  dfd=os.open(scratch,os.O_RDONLY|os.O_DIRECTORY);lat=[];t0=time.monotonic();n=0
  while n<256 and time.monotonic()-t0<5:
   start=time.monotonic_ns()
   if shape=='append_fsync':os.pwrite(fd,b'x'*4096,(n+1)*4096);os.fsync(fd)
   elif shape=='overwrite_fdatasync':os.pwrite(fd,b'x'*4096,0);os.fdatasync(fd)
   else:
    f=os.open(scratch+'/tmp',os.O_CREAT|os.O_WRONLY|os.O_TRUNC,0o600);os.write(f,b'x'*65536);os.fsync(f);os.close(f);os.rename(scratch+'/tmp',scratch+'/target');os.fsync(dfd)
   lat.append((time.monotonic_ns()-start)/1000);n+=1
  os.close(fd);os.close(dfd);lat.sort();rows.append(dict(root=i+1,repeat=r,shape=shape,ops=n,mean_us=statistics.mean(lat),p50_us=lat[n//2],p99_us=lat[min(n-1,n*99//100)],max_us=max(lat)))
  print('sync',rows[-1],flush=True)
(out/'sync.json').write_text(json.dumps(rows,indent=2))
raw=[]
for r in range(3):
 cases=[(i,rw,qd,sync) for i in range(2) for rw in ['write','read'] for qd in [1,16,64] for sync in ([False,True] if rw=='write' and qd==1 else [False])];random.Random(3907+r).shuffle(cases)
 for i,rw,qd,sync in cases:
  with tempfile.TemporaryDirectory(prefix='efs-raw-study-',dir=roots[i]) as scratch:
   name='raw-root%d-%s-qd%d-sync%d-r%d'%(i+1,rw,qd,sync,r);d=out/name
   cmd=[binary,'--bench','io','--storage',scratch,'--rw',rw,'--qd',str(qd),'--window',str(1024//qd),'--io-size','64K','--direct-io','--time','1.5']+(['--preallocate'] if rw=='write' else [])+(['--sync'] if sync else [])
   run=b.execute(cmd,d,'baseline',60);metrics=b.metric_rows(d/'baseline.stdout');raw.append(dict(name=name,root=i+1,repeat=r,rw=rw,qd=qd,sync=sync,run=run,metrics=metrics));print(name,run['returncode'],flush=True)
   (out/'raw.json').write_text(json.dumps(raw,indent=2))
# Short traced reruns are diagnostic only; don't compare their throughput.
for nw in ['0','2','auto']:
 with tempfile.TemporaryDirectory(prefix='efs-strace-study-',dir=roots[0]) as scratch:
  d=out/('strace-w'+nw);d.mkdir();cmd=['strace','-f','-c','-e','trace=futex,openat,newfstatat,access,pwrite64,writev,fdatasync,fsync,rename,renameat','-o',str(d/'summary.txt'),binary,'--bench','data','--storage',scratch,'--rw','write','--qd','64','--window','2','--time','1','--no-direct-io','--full-paths','--writer-stats','--skip-ceiling']+([] if nw=='auto' else ['--writers',nw]);run=b.execute(cmd,d,'trace',60);print('strace',nw,run['returncode'],flush=True)
for name,cmd in [('xfs-root1',['xfs_info','/data1']),('xfs-root2',['xfs_info','/home']),('devices',['lsblk','-o','NAME,MODEL,SERIAL,TYPE,SIZE,ROTA,FSTYPE,MOUNTPOINTS']),('load',['sh','-c','uptime; cat /sys/block/nvme0n1/queue/scheduler /sys/block/nvme1n1/queue/scheduler; cat /sys/block/nvme0n1/queue/write_cache /sys/block/nvme1n1/queue/write_cache'])]:b.execute(cmd,out,name,20)
# Assess scheduling wait on the representative two-writer case.
with tempfile.TemporaryDirectory(prefix='efs-sched-study-',dir=roots[0]) as scratch:
 d=out/'sched-w2';d.mkdir();cmd=['perf','record','-e','sched:sched_switch','-e','sched:sched_wakeup','-o',str(d/'perf.data'),'--',binary,'--bench','data','--storage',scratch,'--rw','write','--qd','64','--window','2','--time','1','--writers','2','--no-direct-io','--full-paths','--writer-stats','--skip-ceiling'];run=b.execute(cmd,d,'record',60)
 if not run['returncode']:b.execute(['perf','sched','timehist','-i',str(d/'perf.data'),'--summary'],d,'timehist',60)
 print('sched',run['returncode'],flush=True)
print('DONE',flush=True)
