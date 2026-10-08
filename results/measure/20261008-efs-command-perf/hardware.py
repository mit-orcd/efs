import concurrent.futures as cf, json, os, pathlib, statistics, subprocess, sys, time, signal
out=pathlib.Path(sys.argv[1]);out.mkdir(exist_ok=True,parents=True)
pids=subprocess.check_output(['pgrep','-f',r'^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split()+subprocess.check_output(['pgrep','-f',r'^./efs-fuse 127.0.0.1:17432 fs /data1/efs/mnt$'],text=True).split()
hz=os.sysconf('SC_CLK_TCK')
def snap():
 cpus={};disks={};procs={};threads={}
 for line in pathlib.Path('/proc/stat').read_text().splitlines():
  v=line.split()
  if v[0].startswith('cpu'):cpus[v[0]]=list(map(int,v[1:9]))
 for line in pathlib.Path('/proc/diskstats').read_text().splitlines():
  v=line.split()
  if v[2] in ('sda','nvme0n1'):disks[v[2]]=list(map(int,v[3:]))
 for pid in pids:
  v=pathlib.Path('/proc/'+pid+'/stat').read_text().rsplit(')',1)[1].split();procs[pid]=[int(v[11]),int(v[12])]
  for p in pathlib.Path('/proc/'+pid+'/task').iterdir():
   try:
    v=(p/'stat').read_text().rsplit(')',1)[1].split();threads[pid+'/'+p.name]=int(v[11])+int(v[12])
   except FileNotFoundError:pass
 net={}
 for line in pathlib.Path('/proc/net/dev').read_text().splitlines()[2:]:
  name,values=line.split(':');v=list(map(int,values.split()));net[name.strip()]=[v[0],v[8]]
 return dict(time=time.monotonic(),cpus=cpus,disks=disks,procs=procs,threads=threads,net=net)
def delta(a,b):
 seconds=b['time']-a['time'];d={'seconds':seconds,'cpu':{},'disks':{},'process_cpu_seconds':{},'net_bytes':{}}
 for k,v in b['cpus'].items():
  x=[z-y for y,z in zip(a['cpus'][k],v)];total=sum(x)
  d['cpu'][k]={'busy_pct':100*(total-x[3]-x[4])/total,'iowait_pct':100*x[4]/total}
 for k,v in b['disks'].items():
  x=[z-y for y,z in zip(a['disks'][k],v)];d['disks'][k]={'read_bytes':512*x[2],'write_bytes':512*x[6],'read_ios':x[0],'write_ios':x[4],'busy_pct':x[9]/(seconds*10),'read_ms':x[3],'write_ms':x[7]}
 for k,v in b['procs'].items():d['process_cpu_seconds'][k]=sum(z-y for y,z in zip(a['procs'][k],v))/hz
 d['top_thread_cpu_seconds']=sorted(((k,(v-a['threads'].get(k,v))/hz) for k,v in b['threads'].items()),key=lambda x:-x[1])[:12]
 for k,v in b['net'].items():d['net_bytes'][k]=[z-y for y,z in zip(a['net'].get(k,v),v)]
 return d
cmd=['/data1/efs/mnt/tiny-files4/.efs/find','file','--match','prefix','--json']
def one(i):
 t=time.monotonic();p=subprocess.run(cmd,capture_output=True,timeout=30);dt=time.monotonic()-t;x=json.loads(p.stdout) if p.returncode==0 else {};n=len(x.get('entries',[]));assert p.returncode==0 and n==25725,(p.returncode,n,p.stderr)
 return dict(seconds=dt,bytes=len(p.stdout),entries=n)
rows=[]
for concurrency,count in [(1,20),(2,24),(4,24)]:
 a=snap()
 with cf.ThreadPoolExecutor(concurrency) as pool:r=list(pool.map(one,range(count)))
 b=snap();d=delta(a,b);d.update(concurrency=concurrency,count=count,median=statistics.median(x['seconds'] for x in r),throughput=count/d['seconds'],samples=r,raw_before=a,raw_after=b);rows.append(d)
 print(json.dumps({k:v for k,v in d.items() if k not in ('raw_before','raw_after','samples')}),flush=True)
(out/'hardware.json').write_text(json.dumps(rows,indent=2))
# Counters collected separately so traced/profiled timings cannot pollute above samples.
f=open(out/'perf-stat.log','w');perf=subprocess.Popen(['perf','stat','-x',';','-e','task-clock,cycles,instructions,cache-references,cache-misses,context-switches,cpu-migrations,page-faults','-p',','.join(pids),'--','sleep','8'],stdout=f,stderr=subprocess.STDOUT)
end=time.monotonic()+7
while time.monotonic()<end:one(0)
perf.wait();f.close()
(out/'hardware-host.txt').write_text(subprocess.check_output(['bash','-c',"lscpu; lsblk -o NAME,MODEL,SIZE,ROTA,MOUNTPOINTS; free -h; df -h /data1 /data2; ps -p "+','.join(pids)+" -o pid,comm,args"],text=True))
