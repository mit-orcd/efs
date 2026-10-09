import sys,os,json,time
sys.path.insert(0,'/data1/efs/src/tests/live')
from query_rpc import request,query
results=[]
for version,term,match,cap in ((2,b'.txt',3,262144),(2,b'.txt',3,1048576),(2,b'.txt',3,16777216),(2,b'file_000000.txt',0,16777216)):
 q=request(version=version,operation=1,root=1,uid=os.getuid(),gid=os.getgid(),term=term,match=match,max_output=cap)
 port=17432;start=time.monotonic()
 for _ in range(4):
  r,text=query(port,q)
  if r.status!=-15:break
  port=17431+r.primary_id
 row={'version':version,'term':term.decode(),'match':match,'max_output':cap,'status':r.status,'checked_dirs':r.checked_dirs,'text_len':r.text_len,'elapsed_s':time.monotonic()-start,'boot_id':r.boot_id,'generation':r.generation}
 if not r.status:
  data=json.loads(text);row['entries']=len(data['entries'])
 results.append(row)
print(json.dumps(results,indent=2))
