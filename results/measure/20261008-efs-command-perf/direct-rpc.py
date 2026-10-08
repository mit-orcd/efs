import concurrent.futures as cf,json,os,pathlib,statistics,sys,time
sys.path.insert(0,'/data1/efs/src/tests/live')
from query_rpc import query,request
out=pathlib.Path(sys.argv[1]);root=os.stat('/data1/efs/mnt/tiny-files4').st_ino
rows=[]
def one(i):
 q=request(version=2,root=root,operation=1,match=1,term=b'file',max_output=4<<20)
 start=time.monotonic();reply,text=query(17433,q);elapsed=time.monotonic()-start
 assert reply.status==0,reply.status
 result=json.loads(text);assert len(result['entries'])==25725
 return {'rpc_seconds':elapsed,'bytes':len(text),'generation':reply.generation}
for concurrency in (1,2,4):
 start=time.monotonic()
 with cf.ThreadPoolExecutor(concurrency) as pool:samples=list(pool.map(one,range(40)))
 dt=time.monotonic()-start
 rows.append(dict(concurrency=concurrency,seconds=dt,throughput=40/dt,median_rpc=statistics.median(x['rpc_seconds'] for x in samples),samples=samples))
 print(concurrency,dt,rows[-1]['throughput'],rows[-1]['median_rpc'],flush=True)
(out/'direct-rpc.json').write_text(json.dumps(rows,indent=2))
