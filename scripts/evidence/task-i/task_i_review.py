#!/usr/bin/env python3
"""Read-only Task I analysis; save derived context without changing verdicts.
Run on nuc: python3 task_i_review.py ~/efs-artifacts/taskI-20261026
"""
import json
from pathlib import Path
import statistics
import sys

art=Path(sys.argv[1]);p=art/'measurement'
doc=json.loads((p/'d4-result.json').read_text());start=doc['measured']['load_started_epoch_s']
host=[json.loads(x) for x in (p/'host.jsonl').read_text().splitlines()]
host=[x for x in host if start<=x['t']<=start+3600]
a,b=host[0],host[-1];span=b['t']-a['t']
cpu=lambda r:list(map(int,r['cpu'].split()[1:9]))
x,y=cpu(a),cpu(b);d=[v-u for u,v in zip(x,y)];total=sum(d)
mem=[];load=[]
for r in host:
 m={line.split(':')[0]:int(line.split()[1]) for line in r['memory'].splitlines()}
 mem.append(m['MemAvailable']/1024/1024);load.append(float(r['load'].split()[0]))
def disks(r):
 out={}
 for line in r['disk'].splitlines():
  f=line.split()
  if len(f)>=14 and (f[2].startswith(('nvme','sd','dm-','md'))):out[f[2]]=list(map(int,f[3:]))
 return out
first,last=disks(a),disks(b);disk={}
for name,v in last.items():
 if name not in first:continue
 u=first[name]
 disk[name]=dict(read_gib=round((v[2]-u[2])*512/2**30,3),write_gib=round((v[6]-u[6])*512/2**30,3),busy_pct=round((v[9]-u[9])/span/10,2))
samples=json.loads((p/'d4-samples.json').read_text());windows=[]
for lo in range(0,3600,600):
 rows=[s for s in samples if lo<=s['t']<lo+600];node={}
 for n in ('1','2','3'):
  pts=[(s['t'],s['nodes'][n]) for s in rows if n in s.get('nodes',{}) and s['nodes'][n].get('data_bytes') is not None]
  if pts:node[n]=dict(min_mib=round(min(v['data_bytes'] for _,v in pts)/2**20,1),max_mib=round(max(v['data_bytes'] for _,v in pts)/2**20,1),last_mib=round(pts[-1][1]['data_bytes']/2**20,1),pending_last=pts[-1][1].get('gc_pending'),samples=len(pts))
 windows.append(dict(start_s=lo,end_s=lo+600,nodes=node))
lastpack=next(s['pack'] for s in reversed(samples) if s.get('pack'))
report=dict(measurement_verdict=doc['verdict'],reason=doc['reason'],host=dict(samples=len(host),span_s=round(span,1),cpu_busy_pct=round(100*(total-d[3]-d[4])/total,2),cpu_iowait_pct=round(100*d[4]/total,2),load1_median=round(statistics.median(load),2),mem_available_gib_min=round(min(mem),2),mem_available_gib_median=round(statistics.median(mem),2),disks=disk),last_pack=lastpack,space_windows=windows)
print(json.dumps(report,indent=2))
