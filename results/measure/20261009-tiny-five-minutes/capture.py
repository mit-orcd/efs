import os,subprocess,time,json,re,signal
from pathlib import Path
base=Path('/data1/efs/tiny-profile-five-minutes');base.mkdir(exist_ok=True)
env=dict(os.environ,SCREENDIR='/run/screen/S-efs')
out='/data1/efs/mnt/tiny-profile-five-minutes'
if not Path(out).exists():
 cmd=f'\x15./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 50000 --workers 8 {out} 2> >(tee {base}/progress.log >&2)\r'
 subprocess.run(['screen','-dmS','tiny_files','bash'],env=env,check=True)
 time.sleep(.5)
 subprocess.run(['screen','-S','tiny_files','-p','0','-X','stuff','\x15cd /home/efs; '+cmd[1:]],env=env,check=True)
 time.sleep(2)
servers=subprocess.check_output(['pgrep','-f','^/data1/efs/src/efsd .*--port 1743[2-5]( |$)']).decode().split()
clients=subprocess.check_output(['pgrep','-f','^./efs-fuse .* /data1/efs/mnt$']).decode().split()
loaders=subprocess.check_output(['pgrep','-f','^python3 ./tiny_files.py .*tiny-profile-five-minutes']).decode().split()
assert len(servers)==4 and len(clients)==1 and len(loaders)==1
pids=servers+clients+loaders
(base/'identity.txt').write_text(subprocess.check_output(['ps','-p',','.join(pids),'-o','pid,etime,pcpu,rss,args']).decode()+subprocess.check_output(['/data1/efs/src/efsd','--version']).decode())
logs={n:Path(f'/data1/efs/logs/efsd-n{n}.log') for n in range(1,5)}
offsets={n:p.stat().st_size for n,p in logs.items()}
record=open(base/'record.log','w')
perf=subprocess.Popen(['perf','record','-F','99','-g','-p',','.join(pids),'-o',str(base/'perf.data'),'--','sleep','300'],stderr=record)
start=time.monotonic()
with open(base/'samples.jsonl','w') as f:
 for i in range(31):
  due=start+i*10
  if time.monotonic()<due:time.sleep(due-time.monotonic())
  sample=dict(seconds=time.monotonic()-start,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),hz=os.sysconf('SC_CLK_TCK'),processes={})
  for pid in pids:
   try:sample['processes'][pid]={name:Path(f'/proc/{pid}/{name}').read_text() for name in ['stat','io','status']}
   except FileNotFoundError:pass
  for name,path in [('diskstats','/proc/diskstats'),('meminfo','/proc/meminfo'),('cpu','/proc/stat'),('pressure_io','/proc/pressure/io'),('pressure_cpu','/proc/pressure/cpu'),('pressure_memory','/proc/pressure/memory')]:
   sample[name]=Path(path).read_text() if Path(path).exists() else None
  text=(base/'progress.log').read_text(errors='replace') if (base/'progress.log').exists() else ''
  matches=list(re.finditer(r'([\d,]+)/([\d,]+) files.*?([\d.]+) s, ([\d,]+) files/s',text))
  if matches:
   m=matches[-1];sample['progress']=dict(created=int(m[1].replace(',','')),target=int(m[2].replace(',','')),elapsed=float(m[3]),cumulative_rate=int(m[4].replace(',','')))
  f.write(json.dumps(sample)+'\n');f.flush()
perf.wait();record.close()
assert perf.returncode==0
# One graceful interrupt stops scheduling; queued writes continue draining.
if subprocess.run(['kill','-0',loaders[0]],stderr=subprocess.DEVNULL).returncode==0:os.kill(int(loaders[0]),signal.SIGINT)
for n,path in logs.items():
 with open(path,'rb') as f:f.seek(offsets[n]);(base/f'n{n}-window.log').write_bytes(f.read())
for pid in pids:
 for label,args in [('self',['--no-children']),('inclusive',['--children'])]:
  with open(base/f'{pid}-{label}.txt','w') as f:subprocess.run(['perf','report','--stdio','-i',str(base/'perf.data'),'--pid',pid,'--percentage','relative','--percent-limit','0.5','--call-graph','none']+args,stdout=f,check=True)
# Three comparable 100-second CPU windows, across all attached processes.
reader=subprocess.Popen(['perf','script','-G','-i',str(base/'perf.data'),'-F','time'],stdout=subprocess.PIPE,text=True)
first=float(reader.stdout.readline().split(':')[0]);reader.terminate();reader.wait()
for label,lo,hi in [('early',0,100),('middle',100,200),('late',200,300)]:
 with open(base/f'{label}-inclusive.txt','w') as f:subprocess.run(['perf','report','--stdio','-i',str(base/'perf.data'),'--time',f'{first+lo:.6f},{first+hi:.6f}','--percentage','relative','--children','--percent-limit','0.5','--call-graph','none'],stdout=f,check=True)
print(base)
