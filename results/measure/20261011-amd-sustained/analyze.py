#!/usr/bin/env python3
"""Describe AMD baseline and candidate; never label unequal runs a matched A/B."""
import json
from pathlib import Path
import re
import statistics
root=Path(__file__).resolve().parent
baseline=json.loads(next((root/'evidence/baseline').glob('gc-direct-*/tiny-sustained.json')).read_text())
stop=json.loads((root/'evidence/baseline/baseline-stop.json').read_text())
counts=[v['close'] for k,v in sorted(baseline['window_counts'].items(),key=lambda x:int(x[0])) if int(k)<10]
assert len(counts)==10
out={'baseline':{'requested_seconds':600,'observed_seconds':baseline['elapsed_s'],
 'intentionally_interrupted':True,'cold_verified':False,
 'completed_close_counts_30s':counts,'mean_close_completions_s':sum(counts)/300,
 'per_node_final_usage_bytes':[int(re.search(r'data_usage_bytes=(\d+)',n['status']).group(1)) for n in stop['nodes']]},
 'candidate':{},'candidate_failures':{},'not_matched_ab':True,'limits':['single AMD host/four local daemons',
 'baseline intentionally interrupted without cold-byte oracle',
 'independent fresh roots/placement and unequal duration; no causal throughput gain',
 'candidate opt-in; production default unchanged',
 '180-second candidate does not close ten-minute or unpaced sustained acceptance']}
for mode in ('direct','buffered'):
 paths=[]
 for p in (root/'evidence/candidate/live').glob(f'gc-{mode}-*/tiny-sustained.json'):
  r=json.loads(p.read_text());
  if r.get('cold_verified') and r.get('physical_inventory_empty'):paths.append((p,r))
  elif 'windows' in r:
   out['candidate_failures'][mode]={'created_and_warm_verified':r['created'],'requested_seconds':r['requested_seconds'],
     'wall_s':r['wall_s'],'physical_inventory_empty':False,
     'mean_full_window_close_completions_s':sum(w['counts']['close'] for w in r['windows'] if w['window']<6)/180,
     'cold_marker_recorded':r.get('cold_verified'),
     'mean_window_close_p99_us':statistics.mean(w['close']['p99_us'] for w in r['windows'] if w['window']<6),
     'mean_window_open_p99_us':statistics.mean(w['open']['p99_us'] for w in r['windows'] if w['window']<6)}
 if not paths:continue
 assert len(paths)==1
 p,r=paths[0]
 assert r['physical_inventory_empty'] and r['created']==r['warm_verified']
 assert r['large_offered_mib_s_per_worker']==2 and r['retained']==512
 assert r['requested_seconds']==180 and r['wall_s']>=180
 windows=[w for w in r['windows'] if w['window']<6]
 assert len(windows)==6
 out['candidate'][mode]={'created_and_warm_verified':r['created'],'retained_cold_verified':r['retained'],
  'large_cold_verified_bytes':4*r['large_live_bytes_each'], 'physical_inventory_empty':True,
  'wall_s':r['wall_s'],'completed_close_counts_30s':[w['counts']['close'] for w in windows],
  'mean_full_window_close_completions_s':sum(w['counts']['close'] for w in windows)/180,
  'mean_window_open_p99_us':statistics.mean(w['open']['p99_us'] for w in windows),
  'mean_window_close_p99_us':statistics.mean(w['close']['p99_us'] for w in windows),
  'peak_sampled_process_rss_kib':max(v['rss_kib'] for sample in r['process_samples'] for v in sample['processes'].values())}
(root/'comparison.json').write_text(json.dumps(out,indent=2)+'\n')
print(json.dumps(out,indent=2))
