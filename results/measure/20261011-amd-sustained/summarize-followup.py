#!/usr/bin/env python3
"""Reconcile complete follow-up gates; independent cells are not matched A/B."""
import json,statistics
from pathlib import Path
root=Path(__file__).resolve().parent
rows=[]
for build,phase in [('fragments','live'),('fanout','live'),('fanout','long')]:
 base=root/'evidence'/build/phase
 for mode in ['direct','buffered']:
  verdict=base/f'{mode}.exit'
  if not verdict.exists():continue
  candidates=list(base.glob(f'gc-{mode}-*/tiny-sustained.json'));assert len(candidates)==1,(build,phase,mode)
  p=candidates[0];r=json.loads(p.read_text())
  if 'requested_seconds' not in r:
   rows.append({'build':build,'phase':phase,'mode':mode,'exit':int(verdict.read_text()),
    'load_incomplete':True,'observed_elapsed_s':r.get('elapsed_s'),'cold_verified':False,
    'physical_inventory_empty':False,'evidence':str(p.relative_to(root))})
   continue
  assert r['created']==r['warm_verified']
  n=r['requested_seconds']//30;windows=[w for w in r['windows'] if w['window']<n];assert len(windows)==n
  rc=int(verdict.read_text());assert rc or (r.get('cold_verified') and r.get('physical_inventory_empty'))
  samples=r['process_samples'];first,last=samples[0],samples[-1]
  cpu=sum(last['processes'][pid]['cpu_ticks']-v['cpu_ticks'] for pid,v in first['processes'].items())/r['cpu_hz']/(last['seconds']-first['seconds'])
  rows.append({'build':build,'phase':phase,'mode':mode,'exit':rc,'seconds':r['requested_seconds'],
   'warm_verified':r['created'],'retained_tiny_cold_checked':r['retained'] if r.get('cold_verified') else None,'large_cold_checked_bytes':4*r['large_live_bytes_each'] if r.get('cold_verified') else None,
   'cold_verified':r.get('cold_verified'),'physical_inventory_empty':r.get('physical_inventory_empty'),
   'cold_recovery_wall_s':r.get('cold_recovery_wall_s'),'gc_drain_wall_s':r.get('gc_drain_wall_s'),
   'full_window_closes_s':sum(w['counts']['close'] for w in windows)/r['requested_seconds'],
   'mean_window_close_p99_us':statistics.mean(w['close']['p99_us'] for w in windows),
   'sampled_five_process_cpu_cores':cpu,'peak_sampled_aggregate_rss_kib':max(sum(v['rss_kib'] for v in s['processes'].values()) for s in samples),
   'evidence':str(p.relative_to(root))})
report={'not_matched_ab':True,'limits':['independent roots and placement','one physical AMD host/four daemons','CPU covers four daemons and FUSE, excludes workload driver','unpaced quota failure not repaired by paced acceptance','no RDMA or hardware power-loss claim'], 'cells':rows}
(root/'followup-summary.json').write_text(json.dumps(report,indent=2)+'\n')
for r in rows:print(r['build'],r['phase'],r['mode'],r['exit'],round(r.get('full_window_closes_s',0),2),r.get('gc_drain_wall_s'))
