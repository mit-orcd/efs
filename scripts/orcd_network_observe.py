#!/usr/bin/env python3
"""Read-only ORCD observer. No remote files, workloads, signals or configuration changes.
Streams a Python observer through the existing login host to fcstor003..013.
"""
import argparse,concurrent.futures,json,pathlib,subprocess
REMOTE=r'''
import pathlib,time,json,os,re,stat,subprocess
seconds=SECONDS;interval=INTERVAL
uid=os.getuid()
def processes():
 out=[]
 for p in pathlib.Path('/proc').iterdir():
  if not p.name.isdigit():continue
  try:
   args=[x.decode() for x in (p/'cmdline').read_bytes().split(b'\0') if x]
   if p.stat().st_uid!=uid or not args or pathlib.Path(args[0]).name not in ('efsd','efs-fuse'):continue
   env=dict(x.decode().split('=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
   keys=('EFS_TRANSPORT','EFS_RDMA_DEV','EFS_COMMANDS','EFS_COMMANDS_SERVER','EFS_PACKED_WRITES','EFS_EXPIRY','EFS_RPC_PROF')
   log=os.readlink(p/'fd/2');pos=None
   if not log.endswith(' (deleted)'):
    try:
     s=pathlib.Path(log).stat()
     if stat.S_ISREG(s.st_mode):pos=s.st_size
    except OSError:pass
   version=subprocess.check_output([str(p/'exe'),'--version'],text=True,timeout=5).strip()
   out.append(dict(pid=int(p.name),args=args,env={k:env.get(k) for k in keys},version=version,log=log,log_offset=pos))
  except (OSError,subprocess.SubprocessError):continue
 return out
paths=[]
for dev in sorted(pathlib.Path('/sys/class/infiniband').glob('*')):
 for port in sorted((dev/'ports').glob('*')):
  for group in ('counters','hw_counters'):
   paths+=list((port/group).glob('*'))
for interface in pathlib.Path('/sys/class/net').glob('*'):
 if interface.name=='lo':continue
 for name in ('rx_packets','tx_packets','rx_bytes','tx_bytes','rx_errors','tx_errors','rx_dropped','tx_dropped','rx_missed_errors'):
  paths.append(interface/'statistics'/name)
def sample():
 out={};errors=[]
 for path in paths:
  try:out[str(path)]=int(path.read_text().strip())
  except (OSError,ValueError) as e:errors.append(str(path)+':'+str(e))
 for name in ('snmp','netstat'):
  lines=pathlib.Path('/proc/net/'+name).read_text().splitlines()
  for i in range(0,len(lines)-1,2):
   a,b=lines[i].split(),lines[i+1].split()
   for k,v in zip(a[1:],b[1:]):
    if k in ('RetransSegs','OutSegs','OutRsts','InErrs','TCPTimeouts','TCPAbortOnTimeout','TCPFastRetrans'):
     out['tcp:'+a[0]+k]=int(v)
 return {'time':time.time(),'counters':out,'errors':errors}
identity=processes();links={}
for dev in pathlib.Path('/sys/class/infiniband').glob('*'):
 for port in (dev/'ports').glob('*'):
  links[str(port)]={}
  for name in ('state','phys_state','rate','link_layer'):
   try:links[str(port)][name]=(port/name).read_text().strip()
   except OSError:pass
rows=[sample()];start=time.monotonic()
while time.monotonic()-start<seconds:
 time.sleep(min(interval,seconds-(time.monotonic()-start)))
 rows.append(sample())
logs=[]
for process in identity:
 offset=process['log_offset']
 if offset is None:continue
 try:
  path=pathlib.Path(process['log']);size=path.stat().st_size
  if size<offset:logs.append({'pid':process['pid'],'rotated':True});continue
  truncated=size-offset>16*1024*1024
  with path.open('rb') as f:
   f.seek(max(offset,size-16*1024*1024));text=f.read(size-f.tell()).decode(errors='replace')
  profiles=re.findall(r'RPC-PROF calls=(\d+).*?busy_n=(\d+)',text)
  logs.append({'pid':process['pid'],'bytes':size-offset,'truncated':truncated,
   'retry_reasons':{reason:len(re.findall(r'inode-rpc: retry .*?why='+reason+r'\b',text)) for reason in ('send','recv','no-hint')},
   'slow_ok_retried':len(re.findall(r'inode-rpc: slow-ok .*attempts=(?:[2-9]|[1-9][0-9]+)\b',text)),
   'exhaustion_lines':len(re.findall(r'exhausted|-> EBUSY|stop-refused|UNMOUNT DATA LOSS',text)),
   'rpc_profiles_first_last':[profiles[0],profiles[-1]] if profiles else []})
 except OSError as e:logs.append({'pid':process['pid'],'error':str(e)})
print(json.dumps({'hostname':os.uname().nodename,'processes_before':identity,'processes_after':processes(),
 'links':links,'samples':rows,'logs':logs}))
'''
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=pathlib.Path,required=True)
 p.add_argument('--seconds',type=int,default=120);p.add_argument('--interval',type=int,default=10)
 a=p.parse_args();assert 10<=a.seconds<=600 and 1<=a.interval<=60
 a.output.mkdir(parents=True,exist_ok=True)
 source=REMOTE.replace('seconds=SECONDS;interval=INTERVAL','seconds='+str(a.seconds)+';interval='+str(a.interval))
 def one(index):
  host='fcstor%03d'%index
  result=subprocess.run(['ssh','orcd-login','ssh','-o','BatchMode=yes','-o','ConnectTimeout=15',host,'python3','-'],input=source,text=True,capture_output=True,timeout=a.seconds+90)
  (a.output/(host+'.stderr')).write_text(result.stderr)
  if result.returncode:raise RuntimeError(host+': '+result.stderr)
  row=json.loads(result.stdout);(a.output/(host+'.json')).write_text(json.dumps(row,indent=2)+'\n')
  first,last=row['samples'][0],row['samples'][-1]
  delta={k:v-first['counters'][k] for k,v in last['counters'].items() if k in first['counters']}
  summary={'host':host,'seconds':last['time']-first['time'],'delta':delta,
   'counter_resets':{k:v for k,v in delta.items() if v<0},'logs':row['logs'],
   'collection_errors':sum(len(s['errors']) for s in row['samples']),
   'process_ids_before':[x['pid'] for x in row['processes_before']],
   'process_ids_after':[x['pid'] for x in row['processes_after']]}
  print(json.dumps({'host':host,'seconds':summary['seconds'],'errors':summary['collection_errors']}),flush=True)
  return summary
 with concurrent.futures.ThreadPoolExecutor(11) as pool:rows=list(pool.map(one,range(3,14)))
 (a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
if __name__=='__main__':main()
