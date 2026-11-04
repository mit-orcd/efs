#!/usr/bin/env python3
"""AC three-repeat medians and min/max spread; histogram quantiles are upper bounds."""
import argparse,json,re,statistics
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('capture',type=Path);a=p.parse_args()
rows=[]
for f in sorted(a.capture.glob('trial-*/result.json')):
 d=json.loads(f.read_text());row={k:d[k] for k in ['variant','transport','mib_s','stat_p50_us','stat_p99_us']}
 row['daemon_cpu_s']=sum(d['daemon_cpu_s'].values());row['rate_late']=sum(d['rate_late'])
 metrics={}
 for item in d['probe']:
  m=re.search(r'ac-probe: scope=(\w+) metric=(\w+) count=(\d+) max=(\d+) bins=([0-9,]+)',item['line']);assert m,item
  key=m[2];hist=[int(v) for v in m[5].split(',')];assert len(hist)==64
  dst=metrics.setdefault(key,dict(count=0,max=0,bins=[0]*64));dst['count']+=int(m[3]);dst['max']=max(dst['max'],int(m[4]));dst['bins']=[x+y for x,y in zip(dst['bins'],hist)]
 for key,v in metrics.items():
  row[key+'_count']=v['count'];row[key+'_max_us']=v['max']/1000
  for q in [.5,.99]:
   cumulative=0
   for b,n in enumerate(v['bins']):
    cumulative+=n
    if cumulative>=v['count']*q:
     row[key+('_p50_upper_us' if q==.5 else '_p99_upper_us')]=((1<<(b+1))-1)/1000;break
 for metric in ['pump_hold_max','lock_wait_max']:
  values=[int(m[1]) for item in d['raft_obs'] if (m:=re.search(r'\b'+metric+r'=(\d+)us',item['line']))]
  if values:row['raft_'+metric+'_us']=max(values)
 row['trial']=f.parent.name;rows.append(row)
summary={}
for variant in ['base','split']:
 group=[r for r in rows if r['variant']==variant];assert len(group)==3,(variant,len(group))
 summary[variant]={}
 for key in sorted(set.intersection(*(set(r) for r in group))):
  if isinstance(group[0][key],(int,float)):
   v=[r[key] for r in group];summary[variant][key]=dict(median=statistics.median(v),min=min(v),max=max(v))
print(json.dumps(dict(trials=rows,summary=summary,limits=[
 'Identical clock/histogram instrumentation adds overhead to both variants.',
 'Poll dispatch measures harvest processing, not NIC completion-to-dispatch time.',
 'Raft printed high-water values cover the whole fixture, including setup; probe histograms cover only the signaled load window.',
 'Existing real-cluster background occupancy is disclosed; these are not quiet-host measurements.'
]),indent=2))
