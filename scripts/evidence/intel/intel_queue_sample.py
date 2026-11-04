#!/usr/bin/env python3
"""Read-only private Intel outbox sampling; gdb reads binary types, never attaches."""
import argparse,concurrent.futures,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',required=True);p.add_argument('--runtime',default='/data1/efs/w100-private/runtime-merged-aa508c820');p.add_argument('--seconds',type=int,default=30);p.add_argument('--output',required=True);a=p.parse_args()
assert 1<=a.seconds<=120
for v in [a.source,a.runtime]:assert v.startswith('/data1/efs/w100-private/') and '..' not in v.split('/')
def one(h):
 code='''import os,pathlib,json,subprocess,re,struct,time,hashlib
src=SOURCE;r=pathlib.Path(RUNTIME);result=[]
for pf in sorted(r.glob('n*.pid')):
 pid=int(pf.read_text());proc=pathlib.Path('/proc')/str(pid)
 assert proc.stat().st_uid==os.getuid() and os.readlink(proc/'exe')==src+'/efsd'
 exe=str(proc/'exe');data=(proc/'exe').read_bytes();typ=struct.unpack('<H',data[16:18])[0]
 expr=['p/x &g_host','p/x &((struct efs_raft_host*)0)->tx','p/x sizeof(struct host_outbox)','p/x &((struct host_outbox*)0)->pending_entries','p/x &((struct host_outbox*)0)->npri','p/x &((struct host_outbox*)0)->nent']
 args=['gdb','-nx','-batch',exe]
 for e in expr:args+=['-ex',e]
 text=subprocess.check_output(args,text=True);vals=[int(v,16) for v in re.findall(r'\\$\\d+ = (0x[0-9a-f]+)',text)];assert len(vals)==7,text
 slide=0
 if typ==3:
  maps=[l.split() for l in (proc/'maps').read_text().splitlines() if src+'/efsd' in l]
  slide=next(int(m[0].split('-')[0],16) for m in maps if int(m[2],16)==0)
 else:assert typ==2,typ
 fd=os.open(proc/'mem',os.O_RDONLY);host=struct.unpack('Q',os.pread(fd,8,slide+vals[0]))[0];assert host
 result.append(dict(pid=pid,sha256=hashlib.sha256(data).hexdigest(),offsets=vals,fd=fd,host=host,samples=[]))
try:
 for i in range(SECONDS*10):
  for d in result:
   _,tx,stride,poff,npri,nent=d['offsets'];fd=d['fd'];host=d['host'];peers=[]
   for peer in range(7):
    addr=host+tx+stride*peer
    pending=struct.unpack('II',os.pread(fd,8,addr+poff))
    peers.append(dict(peer=peer,pending=pending,priority=struct.unpack('i',os.pread(fd,4,addr+npri))[0],entries=struct.unpack('i',os.pread(fd,4,addr+nent))[0]))
   d['samples'].append(dict(sample=i,peers=peers))
  time.sleep(.1)
finally:
 for d in result:os.close(d.pop('fd'));d.pop('host')
print(json.dumps(result))
'''.replace('SOURCE',repr(a.source)).replace('RUNTIME',repr(a.runtime)).replace('SECONDS',str(a.seconds))
 r=subprocess.run(['ssh',h,'python3 -'],input=code,text=True,capture_output=True,timeout=a.seconds+60,check=True);return json.loads(r.stdout)
with concurrent.futures.ThreadPoolExecutor(2) as ex:result=dict(zip(['intel1_efs','intel2_efs'],ex.map(one,['intel1_efs','intel2_efs'])))
from pathlib import Path
Path(a.output).write_text(json.dumps(result)+'\n')
for h,ds in result.items():
 for d in ds:
  print(h,d['pid'],'max pending',max(max(p['pending']) for s in d['samples'] for p in s['peers']),'max entries',max(p['entries'] for s in d['samples'] for p in s['peers']))
