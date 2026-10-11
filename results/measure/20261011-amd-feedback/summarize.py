#!/usr/bin/env python3
"""Keep per-cell load, convergence, cold and physical verdicts separate."""
import json,re,sys
from pathlib import Path
base=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).parent/'evidence'
rows=[]
for case in sorted((base/'pressure').glob('*')):
 if not case.is_dir():continue
 payloads=list(case.glob('gc-*/tiny-sustained.json'))
 if not payloads:continue
 assert len(payloads)==1
 path=payloads[0];r=json.loads(path.read_text());req=r.get('requested_seconds',180)
 if 'windows' not in r:
  rows.append({'case':case.name,'in_progress':True});continue
 ss=r['process_samples'];first=ss[0];last=ss[-1];dt=last['seconds']-first['seconds']
 cpu=sum(last['processes'][p]['cpu_ticks']-value['cpu_ticks'] for p,value in first['processes'].items())/r['cpu_hz']/dt
 recovery=min(ss,key=lambda x:abs(x['seconds']-60))
 changes={k:[a-b for a,b in zip(last[k],recovery[k])] for k in ['tiny_closed_by_worker','large_synced_writes_by_worker']}
 stages=[json.loads(line) for line in (path.parent/'replication-pressure.jsonl').read_text().splitlines()]
 start=stages[0]['mono_us'];end=r['load_settled_mono_us']
 counters={};hist_max={};observed_nodes=0
 for log in path.parent.glob('n*.log'):
  samples=[]
  for line in log.read_text().splitlines():
   match=re.search(r'batch-obs (\{.*\})',line)
   if match:samples.append(json.loads(match.group(1)))
  before=[s for s in samples if s['mono_us']<=start]
  during=[s for s in samples if start<s['mono_us']<=end]
  if not during:continue
  observed_nodes+=1;initial=before[-1]['counters'] if before else {};final=during[-1]['counters']
  for key in ['wire_sent','flow_busy','flow_admitted','flow_legacy','flow_wait','flow_control','flow_probe','inbox_drop','drop_data','drop_empty','flow_enqueue_retry','flow_prebuild_retry']:
   counters[key]=counters.get(key,0)+final[key]-initial.get(key,0)
  for key in ['inbox','inbox_age_us','pump_drain_us','kv_release_us','applied_persist_us']:
   hist_max[key]=max(hist_max.get(key,0),max(s['hist'][key]['max'] for s in during))
 windows=[w for w in r['windows'] if w['window']*30<req]
 p95={key:[w[key]['p95_us'] for w in windows if key in w] for key in ['open','close','delete']}
 rows.append(dict(case=case.name,requested_seconds=req,wall_s=r['wall_s'],successful_tiny_closes=r['created'],
  full_window_closes_s=sum(w['counts'].get('close',0) for w in windows)/req,overload_errors=r['overload_errors'],
  oracle_exit=(case/'run.exit').read_text().strip() if (case/'run.exit').exists() else None,
  cold=r.get('cold_verified',False),physical_empty=r.get('physical_inventory_empty',False),drain_s=r.get('gc_drain_wall_s'),
  catchup=r.get('replication_catchup_verified',False),post_pressure_cold_write=r.get('post_pressure_cold_write_verified',False),
  sampled_cpu_cores=cpu,peak_sampled_rss_kib=max(sum(p['rss_kib'] for p in s['processes'].values()) for s in ss),
  recovery_worker_increments=changes,progress_interval_s=[recovery['seconds'],last['seconds']],
  window_p95_us_ranges={k:[min(v),max(v)] for k,v in p95.items() if v},
  observed_nodes=observed_nodes,sampled_load_counter_deltas=counters,boot_histogram_maxima=hist_max,stages=stages,
  evidence=str(path.relative_to(base))))
pairs=[]
by_case={r['case']:r for r in rows}
for repeat in (2,3,4):
 off=by_case.get(f'direct-off-{repeat}');on=by_case.get(f'direct-on-{repeat}')
 if not off or not on or off.get('in_progress') or on.get('in_progress'):continue
 exposure={}
 for label,cell in [('off',off),('on',on)]:
  phases={v['stage']:v for v in cell['stages']}
  exposure[label]=(phases['healed']['transition_mono_us']-phases['slow']['transition_mono_us'])/1e6
 pairs.append({'repeat':repeat,'slow_exposure_s':exposure,'off_closes_s':off['full_window_closes_s'],'on_closes_s':on['full_window_closes_s'],'paired_change_percent':100*(on['full_window_closes_s']/off['full_window_closes_s']-1)})
result={'paired_direct':pairs,'excluded_pilot_pairs':[1],'cells':rows,'limits':['independent roots/placement and chosen nonleader are performance confounders',
 '180-second synthetic 100-ms slow-sync recovery trials do not identify hardware causes',
 'window p95 ranges are not pooled operation percentiles',
 'histogram maxima are cumulative within the sampled boot, not isolated phase maxima',
 'CPU/RSS includes four daemons and one FUSE client, excludes workload driver',
 'transport admissions and BUSY are never durable consensus ACKs',
 'four private daemons share one physical AMD host; TCP only']}
print(json.dumps(result,indent=2))
