"""Compare only the common initial 180 seconds across unequal-duration cells."""
import json
from pathlib import Path
base=Path(__file__).resolve().parent
def dist(v):
 v=sorted(v);n=len(v)
 return dict(n=n,p50_us=v[(n*50+99)//100-1],p95_us=v[(n*95+99)//100-1],p99_us=v[(n*99+99)//100-1])
result=[]
for tag in ('late','early','late2'):
 cell=base/tag
 if not (cell/'complete.json').exists():continue
 rows=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
 a=min(rows,key=lambda x:abs(x['seconds']));b=min(rows,key=lambda x:abs(x['seconds']-180))
 elapsed=b['seconds']-a['seconds']
 delta=[y-x for x,y in zip(a['progress']['counts'],b['progress']['counts'])]
 probes=json.loads((cell/'read-latency.json').read_text())
 reads={str(g):dist([x['us'] for x in probes if x['stage']=='mixed' and x['group']==g][:37]) for g in (0,2)}
 r=dict(tag=tag,elapsed_seconds=elapsed,files_s=sum(delta)/elapsed,group0_files_s=sum(delta[:4])/elapsed,group2_files_s=sum(delta[4:])/elapsed,reads=reads)
 result.append(r);print(json.dumps(r))
(base/'common-180.json').write_text(json.dumps(result,indent=2))
