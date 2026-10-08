import subprocess,time,json,statistics,pathlib,sys,os,signal
r=sys.argv[1]; root='/data1/efs/mnt/tiny-files4/.efs'; out=pathlib.Path('/data1/efs/command-perf/round'+r);out.mkdir(exist_ok=True)
pids=subprocess.check_output(['pgrep','-f',r'^/data1/efs/src/efsd .*--port 1743[2-5]( |$)'],text=True).split();pids+=subprocess.check_output(['pgrep','-f',r'^./efs-fuse 127.0.0.1:17432 fs /data1/efs/mnt$'],text=True).split()
work=[('stats',['stats','--json']),('ls',['ls','--json']),('exact',['find','file_000000.txt','--match','exact','--json']),('prefix',['find','file','--match','prefix','--json']),('contains',['find','file','--json'])]
rows=[]
for name,args in work:
 for i in range(3):
  t=time.monotonic();p=subprocess.run([root+'/'+args[0]]+args[1:],capture_output=True);dt=time.monotonic()-t
  x=json.loads(p.stdout) if p.returncode==0 else {};rows.append(dict(command=name,seconds=dt,rc=p.returncode,bytes=len(p.stdout),entries=len(x.get('entries',[])),stderr=p.stderr.decode()))
(out/'timings.json').write_text(json.dumps(rows,indent=2))
perf=subprocess.Popen(['perf','record','-F','99','-g','-p',','.join(pids),'-o',str(out/'after.data'),'--','sleep','15'],stdout=open(out/'perf-after.log','w'),stderr=subprocess.STDOUT)
trace=subprocess.Popen(['timeout','-s','TERM','12','strace','-f','-c','-p',pids[-1],'-o',str(out/'fuse-after.strace')],stdout=open(out/'strace-after.log','w'),stderr=subprocess.STDOUT)
end=time.monotonic()+16
while time.monotonic()<end:
 for name,args in work:subprocess.run([root+'/'+args[0]]+args[1:],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
perf.wait();trace.wait()
with open(out/'after.txt','w') as f:subprocess.run(['perf','report','-i',str(out/'after.data'),'--stdio','--no-children','--percent-limit','1'],stdout=f)
subprocess.run(['strace','-f','-tt','-T','-o',str(out/'command-after.strace'),root+'/stats','--json'],stdout=subprocess.DEVNULL,stderr=open(out/'command-after.stderr','w'))
for n,_ in work:
 a=[v['seconds'] for v in rows if v['command']==n];print(n,'median',round(statistics.median(a),4),'max',round(max(a),4),'errors',sum(v['rc']!=0 for v in rows if v['command']==n),flush=True)
