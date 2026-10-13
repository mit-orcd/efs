#!/usr/bin/env python3
import argparse,subprocess,concurrent.futures,json,pathlib
parser=argparse.ArgumentParser(description='Read-only 30-second ORCD EFS TCP socket observation; no remote writes.')
parser.add_argument('--output',type=pathlib.Path,required=True)
a=parser.parse_args();root=a.output;root.mkdir(parents=True,exist_ok=True)
source=r'''
import subprocess,json,re,time,pathlib
def snap():
 text=subprocess.check_output(['ss','-tinpe'],universal_newlines=True);lines=text.splitlines();rows=[]
 for i,line in enumerate(lines):
  if not line.startswith('ESTAB'):continue
  fields=line.split();detail=lines[i+1] if i+1<len(lines) else ''
  if ':17432' not in line and ':17433' not in line:continue
  ino=re.search(r'ino:(\d+)',line)
  retrans=re.search(r'retrans:\d+/(\d+)',detail)
  sent=re.search(r'bytes_sent:(\d+)',detail)
  rows.append({'socket':line,'ino':ino.group(1) if ino else '',
    'retrans':int(retrans.group(1)) if retrans else 0,'bytes_sent':int(sent.group(1)) if sent else 0})
 soft=pathlib.Path('/proc/net/softnet_stat').read_text().splitlines()
 return {'time':time.time(),'sockets':rows,'softnet_dropped':sum(int(x.split()[1],16) for x in soft),'softnet_squeeze':sum(int(x.split()[2],16) for x in soft)}
a=snap();time.sleep(30);b=snap()
old={r['ino']:r for r in a['sockets']};delta=[]
for r in b['sockets']:
 if r['ino'] in old:
  o=old[r['ino']];delta.append({'socket':r['socket'],'retrans':r['retrans']-o['retrans'],'bytes_sent':r['bytes_sent']-o['bytes_sent']})
print(json.dumps({'before':a,'after':b,'stable_socket_deltas':delta,'softnet_dropped':b['softnet_dropped']-a['softnet_dropped'],'softnet_squeeze':b['softnet_squeeze']-a['softnet_squeeze']}))
'''
def one(host):
 r=subprocess.run(['ssh','orcd-login','ssh','-o','BatchMode=yes',host,'python3','-'],input=source,text=True,capture_output=True,check=True,timeout=75)
 (root/(host+'-tcp-followup.json')).write_text(r.stdout)
 data=json.loads(r.stdout); print(host,json.dumps({'retrans':sum(x['retrans'] for x in data['stable_socket_deltas']),'bytes_sent':sum(x['bytes_sent'] for x in data['stable_socket_deltas']),'softnet_dropped':data['softnet_dropped'],'softnet_squeeze':data['softnet_squeeze']}))
with concurrent.futures.ThreadPoolExecutor(2) as pool:list(pool.map(one,['fcstor004','fcstor005']))
