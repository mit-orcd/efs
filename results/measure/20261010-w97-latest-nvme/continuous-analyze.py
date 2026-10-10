"""Offline continuous-load / drained-idle / same-directory-resume evidence."""
import json,re
from pathlib import Path
base=Path(__file__).resolve().parent;cell=base/'continuous'
samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
phases=json.loads((cell/'phases.json').read_text());final=json.loads((cell/'load-final.json').read_text())
assert final['errors']==[] and max(final['counts'])<128000
assert phases['pause_drained_us'] is not None
windows=[]
for lo,hi in [(i,i+300) for i in range(0,1200,300)]+[(1200,1800)]+[(i,i+300) for i in range(1800,3000,300)]:
 a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds'];counts=[y-x for x,y in zip(a['progress']['counts'],b['progress']['counts'])]
 def ds(text):return {v[2]:list(map(int,v[3:])) for v in (line.split() for line in text.splitlines())}
 old,new=ds(a['diskstats']),ds(b['diskstats']);disks={}
 for name in ('sda','nvme0n1'):
  d=[y-x for x,y in zip(old[name],new[name])];ios=d[0]+d[4]
  disks[name]=dict(host_write_GiB=d[6]*512/2**30,read_MiB_s=d[2]*512/dt/2**20,write_MiB_s=d[6]*512/dt/2**20,mean_request_ms=(d[3]+d[7])/ios if ios else None,io_busy_percent=d[9]/dt/10,flush_requests_s=d[15]/dt if len(d)>16 else None,mean_flush_ms=d[16]/d[15] if len(d)>16 and d[15] else None)
 windows.append(dict(start=a['seconds'],end=b['seconds'],files_s=sum(counts)/dt,group0_files_s=sum(counts[:4])/dt,group2_files_s=sum(counts[4:])/dt,disks=disks))
rss={};lag={0:0,2:0}
for s in samples:
 for pid,v in s['processes'].items():
  m=re.search(r'^VmRSS:\s+(\d+) kB',v['status'],re.M)
  if m:rss[pid]=max(rss.get(pid,0),int(m[1]))
 for r in s['raft_status']:
  if r['hosted']:lag[r['group']]=max(lag[r['group']],r['commit']-r['applied'])
result=dict(windows=windows,phases=phases,latency=final['latency'],max_sampled_rss_kib=rss,max_sampled_local_commit_minus_applied=lag,errors=final['errors'],counts=final['counts'])
(cell/'continuous-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
