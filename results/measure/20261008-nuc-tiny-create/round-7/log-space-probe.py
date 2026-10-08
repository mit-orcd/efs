import os,time,json,signal,statistics
pid=1661838
results=[]
os.kill(pid,signal.SIGSTOP)
try:
 for base in ['/data1/efs/profile-tiny-20261008','/data2/efs']:
  for mode in ['append','zeroed','fallocate','append','zeroed']:
   path=f'{base}/r7-log-probe-{mode}-{time.time_ns()}'
   fd=os.open(path,os.O_RDWR|os.O_CREAT|os.O_EXCL,0o600)
   pre=time.perf_counter()
   if mode=='zeroed': os.pwrite(fd,b'\0'*(1024*1024),0)
   if mode=='fallocate': os.posix_fallocate(fd,0,1024*1024)
   os.fdatasync(fd)
   prep=time.perf_counter()-pre
   samples=[]
   for i in range(128):
    start=time.perf_counter()
    os.pwrite(fd,b'x'*1024,i*1024)
    os.fdatasync(fd)
    samples.append((time.perf_counter()-start)*1000)
   os.close(fd)
   results.append(dict(base=base,mode=mode,prep_ms=prep*1000,mean_ms=statistics.mean(samples),median_ms=statistics.median(samples),p95_ms=sorted(samples)[121],path=path))
 print(json.dumps(results,indent=2),flush=True)
finally: os.kill(pid,signal.SIGCONT)
