"""Offline snapshots, actual worker latencies and per-group interval rates."""
import json,math
from pathlib import Path
base=Path(__file__).resolve().parent
def dist(values):
 a=sorted(values);n=len(a)
 return dict(n=n,mean=sum(a)/n if n else None,**{f'p{q}':a[min(n-1,(n*q+99)//100-1)] if n else None for q in (50,95,99)},max=a[-1] if n else None)
all_results=[]
for tag in ('control1','candidate1','candidate2','control2'):
 cell=base/tag
 if not (cell/'complete.json').exists():continue
 t=json.loads((cell/'timebase.json').read_text());start=t['start_mono_us'];duration=t['duration_seconds'];final=json.loads((cell/'load-final.json').read_text())
 samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
 rates=[]
 for lo,hi in ((0,duration//3),(duration//3,2*duration//3),(2*duration//3,duration)):
  a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds']
  deltas=[y-x for x,y in zip(a['progress']['counts'],b['progress']['counts'])]
  rates.append(dict(start=a['seconds'],end=b['seconds'],files_s=sum(deltas)/dt,group0_files_s=sum(deltas[:4])/dt,group2_files_s=sum(deltas[4:])/dt))
 cap_workers=[i for i,n in enumerate(final['counts']) if n>=128000]
 result=dict(tag=tag,duration_seconds=duration,rates=rates,latency=final['latency'],workers=final['workers'],mean_files_s=sum(samples[-1]['progress']['counts'])/duration,nodes=[],reads={},worker_cap_hit=cap_workers,valid_unclipped_performance=not cap_workers)
 probes=json.loads((cell/'read-latency.json').read_text())
 for stage in ('idle_before','mixed','idle_after'):
  result['reads'][stage]={str(g):dist([r['us'] for r in probes if r['stage']==stage and r['group']==g]) for g in (0,2)}
 for n in range(1,5):
  rows=[json.loads(x) for x in (cell/f'n{n}-observations.jsonl').read_text().splitlines()]
  a=max((r for r in rows if r['mono_us']<=start),key=lambda r:r['mono_us']);b=max((r for r in rows if r['mono_us']<=start+duration*1e6),key=lambda r:r['mono_us'])
  c={k:b['counters'][k]-v for k,v in a['counters'].items()};h={}
  for k,v in a['hist'].items():
   count=b['hist'][k]['n']-v['n'];total=b['hist'][k]['sum']-v['sum'];bins=[y-x for x,y in zip(v['bins'],b['hist'][k]['bins'])]
   h[k]=dict(n=count,sum=total,mean=total/count if count else None,bins=bins,bin_count_delta=sum(bins)-count)
  commands={k:b['commands'][k]-v for k,v in a['commands'].items() if b['commands'][k]!=v}
  result['nodes'].append(dict(node=n,commands=commands,counters=c,hist=h,start_seconds=(a['mono_us']-start)/1e6,end_seconds=(b['mono_us']-start)/1e6))
 (cell/'analysis.json').write_text(json.dumps(result,indent=2));all_results.append(result)
(base/'balanced-summary.json').write_text(json.dumps(all_results,indent=2))
for r in all_results:
 print(r['tag'],'mean',r['mean_files_s'],'windows',r['rates'],'latency',r['latency'])
 for n in r['nodes']:
  h=n['hist'];print(n['node'],'entries/sync',h['wal_entries']['mean'],'overlap',n['counters']['batch_overlap'],'errors',n['counters']['sync_error'],'checkpoint', {k:n['counters'][k] for k in ('applied_sync','applied_error','applied_throttle','applied_entry_cap')},'phases_s',{k:h[k]['sum']/1e6 for k in ('pump_drain_us','kv_release_us','applied_persist_us')})
 print('reads',r['reads'])
