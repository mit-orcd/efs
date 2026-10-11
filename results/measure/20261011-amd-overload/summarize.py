#!/usr/bin/env python3
"""Keep load admission, acknowledged-byte recovery and cleanup verdicts separate."""
import datetime,json,re,statistics
from pathlib import Path
b=Path(__file__).resolve().parent
rows=[]
for phase in ['live','long']:
 for case in ['control-direct','admission-direct','admission-buffered']:
  folder=b/'evidence'/phase/case
  verdict=folder/'run.exit'
  if not verdict.exists(): continue
  files=list(folder.glob('gc-*/tiny-sustained.json'));assert len(files)==1
  p=files[0];r=json.loads(p.read_text());assert 'requested_seconds' in r
  rc=int(verdict.read_text());errors=r.get('overload_errors',[])
  load_pass=not errors and r['wall_s']>=r['requested_seconds']
  if load_pass:assert r['created']==r['warm_verified']
  oracle=rc==0 and r.get('cold_verified') and r.get('physical_inventory_empty')
  if oracle:assert r['cold_acknowledged_tiny']>0 and r['cold_acknowledged_large_bytes']>0
  samples=r['process_samples'];first,last=samples[0],samples[-1]
  cpu=sum(last['processes'][pid]['cpu_ticks']-v['cpu_ticks'] for pid,v in first['processes'].items())/r['cpu_hz']/(last['seconds']-first['seconds'])
  n=r['requested_seconds']//30
  windows=[w for w in r['windows'] if w['window']<n]
  progress=None
  if load_pass and 'tiny_closed_by_worker' in last:
   earlier=min(samples,key=lambda s:abs(s['seconds']-(r['requested_seconds']-150)))
   progress={key:[a-z for a,z in zip(last[key],earlier[key])]
             for key in ['tiny_closed_by_worker','large_synced_writes_by_worker']}
   assert all(all(v>0 for v in values) for values in progress.values()),progress
  stamps=[]
  for log in p.parent.glob('n*.log'):
   for stamp in re.findall(r'^([0-9]{4}-[^ ]+Z)',log.read_text(),re.M):
    stamps.append(datetime.datetime.fromisoformat(stamp.replace('Z','+00:00')).timestamp())
  usage=[0]*4
  sf=b/'evidence'/('status-samples.jsonl' if phase=='live' else 'long-status-samples.jsonl')
  if stamps and sf.exists():
   for line in sf.read_text().splitlines():
    sample=json.loads(line)
    if min(stamps)<=sample['epoch']<=max(stamps):
     for node in sample['nodes']:
      m=re.search(r'data_usage_bytes=(\d+)',node.get('status',''))
      if m:usage[node['port']-24210]=max(usage[node['port']-24210],int(m[1]))
  rows.append(dict(phase=phase,case=case,oracle_exit=rc,load_pass=load_pass,
     cold_and_physical_pass=bool(oracle),seconds_requested=r['requested_seconds'],wall_s=r['wall_s'],
     successful_tiny_closes=r['created'],overload_errnos=[e['errno'] for e in errors],
     cold_tiny=r.get('cold_acknowledged_tiny'),cold_large_bytes=r.get('cold_acknowledged_large_bytes'),
     gc_drain_s=r.get('gc_drain_wall_s'),post_pressure_cold_write=r.get('post_pressure_cold_write_verified'),
     full_window_closes_s=sum(w['counts']['close'] for w in windows)/r['requested_seconds'] if load_pass else None,
     sampled_cpu_cores=cpu,peak_sampled_rss_kib=max(sum(v['rss_kib'] for v in s['processes'].values()) for s in samples),
     peak_sampled_node_charged_bytes=usage,last_observed_worker_progress=progress,worker_progress_interval_s=[earlier['seconds'],last['seconds']] if progress else None,
     evidence=str(p.relative_to(b))))
report={'cells':rows,'limits':['independent roots/salts: not matched throughput A/B','logical-quota experiment, not W99 filesystem/capacity-domain policy','sampled usage is not an all-instant physical-space bound','four daemons on one physical AMD host','CPU includes four daemons/FUSE, excludes workload driver','large oracle uses constant bodies and acknowledged fsync prefixes, not newest-generation discrimination']}
(b/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
for r in rows:print(r['phase'],r['case'],'load',r['load_pass'],'cold/physical',r['cold_and_physical_pass'],'closes',r['successful_tiny_closes'],'drain',r['gc_drain_s'])
