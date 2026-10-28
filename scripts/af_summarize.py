#!/usr/bin/env python3
"""Three-repeat AF medians/min/max; histogram quantiles are upper bounds."""
import argparse,json,re,statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);a=p.parse_args()
rows=[]
for f in sorted(a.capture.glob('trial-*/result.json')):
 d=json.loads(f.read_text());assert d['verdict']=='PASS',f
 row=dict(trial=f.parent.name,hz=d['hz'],mib_s=d['mib_s'],daemon_cpu_s=sum(d['daemon_cpu_s'].values()),
          stat_p50_us=d['stat']['p50_us'],stat_p99_us=d['stat']['p99_us'],write_late=sum(d['write_late']))
 for q in ['p50_us','p99_us']:
  row['status_node_'+q]=max(v[q] for v in d['status'].values())
 row['polls']=sum(v['count'] for v in d['status'].values())
 metrics={}
 for item in d['probe']:
  m=re.search(r'af-probe metric=(\w+) count=(\d+) max=(\d+) bins=([0-9,]+)',item['line']);assert m,item
  hist=[int(v) for v in m[4].split(',')];assert len(hist)==64
  dst=metrics.setdefault(m[1],dict(count=0,max=0,bins=[0]*64));dst['count']+=int(m[2]);dst['max']=max(dst['max'],int(m[3]));dst['bins']=[x+y for x,y in zip(dst['bins'],hist)]
 for key,v in metrics.items():
  row[key+'_count']=v['count'];row[key+'_max_us']=v['max']/1000
  for q in [.5,.99]:
   count=0
   for b,n in enumerate(v['bins']):
    count+=n
    if count>=v['count']*q:row[key+('_p50_upper_us' if q==.5 else '_p99_upper_us')]=((1<<(b+1))-1)/1000;break
 for key in ['pump_hold_max','lock_wait_max']:
  values=[int(m[1]) for item in d['raft_obs'] if (m:=re.search(r'\b'+key+r'=(\d+)us',item['line']))]
  if values:row['raft_'+key+'_us']=max(values)
 rows.append(row)
summary={}
for hz in [1,10,50]:
 group=[r for r in rows if r['hz']==hz];assert len(group)==3,(hz,len(group));summary[hz]={}
 for k in set.intersection(*(set(r) for r in group)):
  if isinstance(group[0][k],(float,int)):
   v=[r[k] for r in group];summary[hz][k]=dict(median=statistics.median(v),min=min(v),max=max(v))
print(json.dumps(dict(trials=rows,summary=summary,limits=[
 'Same instrumented source at all rates; bounded atomic/clock probe overhead included.',
 'Pump period includes event/timer sleep; pump work excludes that sleep and includes mutex wait.',
 'raft-obs printed high-water spans fixture setup; probes are limited to signaled load window.',
 'Status RPC latency includes efs-mgmt process launch, connection and parsing; server lock probes exclude those.',
 'Fixed-rate D4-derived rewrite diagnostic, not full D4/soak qualification; background client disclosed.'
]),indent=2,sort_keys=True))
