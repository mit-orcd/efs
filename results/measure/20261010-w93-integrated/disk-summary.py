"""Host block request deltas, not per-file attribution or a drive ceiling."""
import json
from pathlib import Path
base=Path(__file__).resolve().parent
rows=[]
def parse(text):
 return {v[2]:list(map(int,v[3:])) for v in (line.split() for line in text.splitlines())}
for tag in ('control1','candidate1','candidate2','control2'):
 p=base/tag/'samples.jsonl'
 if not (base/tag/'complete.json').exists():continue
 samples=[json.loads(v) for v in p.read_text().splitlines()]
 for lo,hi in ((0,samples[-1]['seconds']/3),(samples[-1]['seconds']/3,2*samples[-1]['seconds']/3),(2*samples[-1]['seconds']/3,samples[-1]['seconds'])):
  a=min(samples,key=lambda x:abs(x['seconds']-lo));b=min(samples,key=lambda x:abs(x['seconds']-hi));dt=b['seconds']-a['seconds']
  old=parse(a['diskstats']);new=parse(b['diskstats'])
  for name in ('sda','nvme0n1','nvme1n1'):
   if name not in old or name not in new:continue
   d=[y-x for x,y in zip(old[name],new[name])];ios=d[0]+d[4]
   rows.append(dict(tag=tag,device=name,start_s=a['seconds'],end_s=b['seconds'],read_MiB_s=d[2]*512/dt/2**20,write_MiB_s=d[6]*512/dt/2**20,requests_s=ios/dt,
    mean_read_write_request_ms=(d[3]+d[7])/ios if ios else None,io_busy_percent=d[9]/dt/10,weighted_io_ms_s=d[10]/dt,
    flush_requests_s=d[15]/dt if len(d)>=17 else None,mean_flush_ms=d[16]/d[15] if len(d)>=17 and d[15] else None))
(base/'disk-summary.json').write_text(json.dumps(rows,indent=2))
for row in rows:print(json.dumps(row))
