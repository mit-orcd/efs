"""Offline bounds and command/pressure summary from retained timed snapshots."""
import json,re
from pathlib import Path
base=Path(__file__).resolve().parent
all=[]
for tag in ('late','early','late2'):
 cell=base/tag
 if not (cell/'analysis.json').exists():continue
 analysis=json.loads((cell/'analysis.json').read_text())
 samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
 rss={}
 for row in samples:
  for pid,values in row['processes'].items():
   m=re.search(r'^VmRSS:\s+(\d+) kB',values['status'],re.M)
   if m:rss[pid]=max(rss.get(pid,0),int(m[1]))
 bounds=[]
 for node in analysis['nodes']:
  h=node['hist'];c=node['counters']
  bounds.append(dict(node=node['node'],inbox_mean_frames=h['inbox']['mean'],queue_mean_us=h['inbox_age_us']['mean'],inbox_refused=c['inbox_drop'],wire_busy=c['flow_busy'],early_retire=c['flow_retire_early'],sync_errors=c['sync_error']))
 record=dict(tag=tag,max_sampled_rss_kib=rss,nodes=bounds,worker_errors=json.loads((cell/'load-final.json').read_text())['errors'])
 all.append(record)
 print(json.dumps(record))
(base/'resource-summary.json').write_text(json.dumps(all,indent=2))
