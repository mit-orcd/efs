"""Offline diagnostic deltas: physical append work is not unique client ops."""
from pathlib import Path
import json,re
base=Path(__file__).resolve().parent
header=Path(__file__).resolve().parents[3]/'include/efs/meta_cmd.h'
# Running remotely? Retain a local copy of the namespace with the evidence.
if not header.exists():header=base/'meta_cmd.h'
names={int(m[2]):m[1].removeprefix('EFS_MD_CMD_') for m in re.finditer(r'#define (EFS_MD_CMD_\w+)\s+(\d+)\b',header.read_text())}
names.update({0:'RAFT_NOOP',193:'RAFT_JOINT',194:'RAFT_COLD'})
summary=[]
for tag in ('baseline','noop','both','group'):
 d=base/tag
 if not (d/'trial.json').exists():continue
 times=json.loads((d/'timebase.json').read_text());start=times['start_mono_us'];duration=times['duration_seconds']
 latency=json.loads((d/'latency-summary.json').read_text());files=sum(r['n'] for r in latency.values())
 rate=json.loads((d/'rates.json').read_text());result=dict(tag=tag,mean_files_s=sum(r['files_per_second'] for r in rate)/len(rate),completed_files=files,
 file_latency_mean_ms=sum(r['sum_us'] for r in latency.values())/files/1000,nodes=[])
 analysis=json.loads((d/'analysis.json').read_text())
 for node in range(1,5):
  rows=[json.loads((d/f'baseline-n{node}.json').read_text())]+[json.loads(l) for l in (d/f'n{node}-observations.jsonl').read_text().splitlines()]
  a=max((r for r in rows if r['mono_us']<=start),key=lambda r:r['mono_us'])
  b=max((r for r in rows if r['mono_us']<=start+duration*1000000),key=lambda r:r['mono_us'])
  commands={names.get(int(k),k):b['commands'][k]-v for k,v in a['commands'].items() if b['commands'][k]!=v}
  r=next(r for r in analysis if r['node']==node and r['window']=='all')
  out=dict(node=node,commands=commands,append_commands_per_completed_file=sum(commands.values())/files,
   syncs=r['counters']['sync'],noop_continuations=r['counters']['batch_noop'],group_boundaries=r['counters']['host_group'],
   phase_seconds={k:r['hist'][k]['sum']/1e6 for k in ('pump_drain_us','kv_release_us','applied_persist_us')},
   raft_entries_per_sync=r['hist']['wal_entries']['mean'],sync_mean_ms=r['hist']['sync_us']['mean']/1000,
   inbox_mean_ms=r['hist']['inbox_age_us']['mean']/1000)
  result['nodes'].append(out)
 summary.append(result)
(base/'efficiency-summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
