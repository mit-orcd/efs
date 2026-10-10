"""Offline bounds and command/pressure summary from retained timed snapshots."""
import json,re
from pathlib import Path
base=Path(__file__).resolve().parent
all=[]
for tag in ('control1','candidate1','candidate2','control2'):
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
 lag={g:dict(max_commit_minus_applied=0,max_replica_commit_spread=0,max_replica_applied_spread=0) for g in (0,2)}
 for sample in samples:
  for g in (0,2):
   rows=[r for r in sample["raft_status"] if r["hosted"] and r["group"]==g]
   if rows:
    b=lag[g];b["max_commit_minus_applied"]=max(b["max_commit_minus_applied"],max(r["commit"]-r["applied"] for r in rows))
    for field in ("commit","applied"):
     b["max_replica_"+field+"_spread"]=max(b["max_replica_"+field+"_spread"],max(r[field] for r in rows)-min(r[field] for r in rows))
 sync={n:{v[2]:int(v[0]) for v in (line.split(",") for line in (cell/f"n{n}-sync.csv").read_text().splitlines()) if len(v)>2 and v[2].startswith("syscalls:")} for n in range(1,5)}
 record=dict(sync_syscalls=sync,replica_lag=lag,structural_bounds=dict(inbox_frames=256,inbox_bytes=32<<20,apply_effect_keys=32768,apply_effect_bytes=64<<20),tag=tag,max_sampled_rss_kib=rss,nodes=bounds,worker_errors=json.loads((cell/'load-final.json').read_text())['errors'])
 all.append(record)
 print(json.dumps(record))
(base/'resource-summary.json').write_text(json.dumps(all,indent=2))
