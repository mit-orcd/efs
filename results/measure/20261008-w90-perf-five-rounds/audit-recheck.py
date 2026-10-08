from pathlib import Path
import json,re
root=Path('/data1/efs/bench/w90-perf5-20261008')
for d in root.glob('strace-*'):
 x=json.loads((d/'summary.json').read_text());f=x['fields']
 assert f['dead_records']=='0' and f['errors']=='0' and f['live_objects']==f['verified_objects']
 checked=0
 for line in (d/'measured-calls.txt').open():
  if re.search(r' (pread64|pwrite64)\(',line):
   m=re.search(r', (\d+)\s*\)\s+=',line);assert m,line
   off=int(m.group(1));assert off%69632==0 and off+69632<=1024**3,line;checked+=1
  elif ' fallocate(' in line:
   m=re.search(r', (\d+), (\d+)\s*\)\s+=',line);assert m,line
   off,length=map(int,m.groups());assert off%69632==length%69632==0 and off+length<=1024**3,line;checked+=1
 assert checked==int(f['reads'])+int(f['writes'])+x['counts']['fallocate']
 print(d.name,'aligned I/O and punch bounds PASS;',checked,'calls;',x['directory_syncs'],'directory fsyncs')
