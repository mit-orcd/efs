#!/usr/bin/env python3
"""Read-only EFS TCP/IPoIB sender/receiver observation. Writes only local evidence."""
import argparse,concurrent.futures,json,pathlib,subprocess
REMOTE=r'''
import pathlib,time,json,subprocess,re,os
seconds=SECONDS;interval=INTERVAL
interface='ibs1f0'
def read(path):
 try:return pathlib.Path(path).read_text().strip()
 except OSError as e:return {'unavailable':str(e)}
def sample():
 counters={}
 for name in ('snmp','netstat'):
  lines=pathlib.Path('/proc/net/'+name).read_text().splitlines()
  for i in range(0,len(lines)-1,2):
   a,b=lines[i].split(),lines[i+1].split()
   for k,v in zip(a[1:],b[1:]):counters[a[0]+k]=int(v)
 for group in ('counters','hw_counters'):
  for p in pathlib.Path('/sys/class/infiniband/mlx5_2/ports/1',group).glob('*'):
   value=read(p)
   if isinstance(value,str) and value.isdigit():counters['IB:'+p.name]=int(value)
 for p in pathlib.Path('/sys/class/net',interface,'statistics').glob('*'):
  value=read(p)
  if isinstance(value,str) and value.isdigit():counters['NIC:'+p.name]=int(value)
 soft=pathlib.Path('/proc/net/softnet_stat').read_text().splitlines()
 counters['softnet:dropped']=sum(int(x.split()[1],16) for x in soft)
 counters['softnet:time_squeeze']=sum(int(x.split()[2],16) for x in soft)
 text=subprocess.check_output(['ss','-tinpe'],universal_newlines=True)
 lines=text.splitlines();sockets={}
 for i,line in enumerate(lines):
  if not line.startswith('ESTAB') or not any(':'+port in line for port in ('17432','17433')):continue
  ino=re.search(r'ino:(\d+)',line)
  if not ino:continue
  detail=lines[i+1] if i+1<len(lines) else ''
  metrics={k:int(v) for k,v in re.findall(r'\b(bytes_sent|bytes_received|bytes_retrans|segs_out|data_segs_out|dsack_dups|reord_seen):(\d+)',detail)}
  retrans=re.search(r'\bretrans:(\d+)/(\d+)',detail)
  metrics['retrans_total']=int(retrans.group(2)) if retrans else 0
  sockets[ino.group(1)]={'socket':line,'info':detail.strip(),'metrics':metrics}
 return {'time':time.time(),'counters':counters,'sockets':sockets}
def extended_stats():
 text=subprocess.check_output(['ethtool','-S',interface],universal_newlines=True)
 return {k:int(v) for k,v in re.findall(r'^\s*([\w]+):\s*(\d+)\s*$',text,re.M)}
config={name:read('/sys/class/net/'+interface+'/'+name) for name in ('mode','mtu','tx_queue_len')}
config['extended_before']=extended_stats()
config['offloads']=subprocess.run(['ethtool','-k',interface],stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True).stdout
config['qdisc']=subprocess.run(['tc','-s','qdisc','show','dev',interface],stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True).stdout
identity=[]
for p in pathlib.Path('/proc').iterdir():
 try:
  args=[x.decode() for x in (p/'cmdline').read_bytes().split(b'\0') if x]
  if not args or pathlib.Path(args[0]).name!='efsd' or p.stat().st_uid!=os.getuid():continue
  env=dict(x.decode().split('=',1) for x in (p/'environ').read_bytes().split(b'\0') if b'=' in x)
  identity.append({'pid':p.name,'args':args,'transport':env.get('EFS_TRANSPORT'),'rdma_dev':env.get('EFS_RDMA_DEV'),'exe':os.readlink(p/'exe')})
 except (OSError,NotADirectoryError):pass
rows=[sample()];start=time.monotonic()
while time.monotonic()-start<seconds:
 time.sleep(max(0,min(interval,seconds-(time.monotonic()-start))))
 rows.append(sample())
config['extended_after']=extended_stats()
config['qdisc_after']=subprocess.run(['tc','-s','qdisc','show','dev',interface],stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True).stdout
print(json.dumps({'host':os.uname().nodename,'identity':identity,'config':config,'samples':rows}))
'''
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=pathlib.Path,required=True)
p.add_argument('--seconds',type=int,default=120);p.add_argument('--interval',type=int,default=5)
a=p.parse_args();assert 10<=a.seconds<=300 and 1<=a.interval<=30
a.output.mkdir(parents=True,exist_ok=True)
source=REMOTE.replace('seconds=SECONDS;interval=INTERVAL','seconds='+str(a.seconds)+';interval='+str(a.interval))
def one(host):
 r=subprocess.run(['ssh','orcd-login','ssh','-o','BatchMode=yes',host,'python3','-'],input=source,text=True,capture_output=True,check=True,timeout=a.seconds+90)
 row=json.loads(r.stdout);(a.output/(host+'.json')).write_text(json.dumps(row,indent=2)+'\n')
 first,last=row['samples'][0],row['samples'][-1]
 delta={k:v-first['counters'][k] for k,v in last['counters'].items() if k in first['counters']}
 events=[]
 for old,new in zip(row['samples'],row['samples'][1:]):
  for ino,entry in new['sockets'].items():
   if ino not in old['sockets']:continue
   previous=old['sockets'][ino]['metrics'];metrics=entry['metrics']
   n=metrics['retrans_total']-previous['retrans_total']
   if n:events.append({'start':old['time'],'end':new['time'],'ino':ino,'retrans_delta':n,'bytes_retrans_delta':metrics.get('bytes_retrans',0)-previous.get('bytes_retrans',0),'socket':entry['socket'],'info':entry['info']})
 result={'host':host,'seconds':last['time']-first['time'],'delta':delta,'retransmit_events':events}
 print(json.dumps({'host':host,'retrans':delta.get('Tcp:RetransSegs'),'timeouts':delta.get('TcpExt:TCPTimeouts'),'events':len(events)}),flush=True)
 return result
with concurrent.futures.ThreadPoolExecutor(3) as pool:rows=list(pool.map(one,['fcstor003','fcstor004','fcstor005']))
(a.output/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
