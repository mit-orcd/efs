"""Offline resource maxima and sampled replica lag; no live probes."""
import json,re
from pathlib import Path
base=Path(__file__).resolve().parent
out={}
for tag in ('baseline','overlap'):
 cell=base/tag
 if not (cell/'complete.json').exists():continue
 rows=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
 identity=json.loads((cell/'identity.json').read_text());labels={str(s['pid']):f'n{n}' for n,s in identity['servers'].items()}
 rss={};lag=[];spaces=[];commands={}
 t=json.loads((cell/'timebase.json').read_text());start=t['start_mono_us'];names=json.loads((base/'command-opcodes.json').read_text())
 for n in range(1,5):
  obs=[json.loads(x) for x in (cell/f'n{n}-observations.jsonl').read_text().splitlines()]
  a=max((r for r in obs if r['mono_us']<=start),key=lambda r:r['mono_us']);b=max((r for r in obs if r['mono_us']<=start+900000000),key=lambda r:r['mono_us'])
  commands[f'n{n}']={names.get(k,'opcode_'+k):b['commands'][k]-v for k,v in a['commands'].items() if b['commands'][k]!=v}
 for r in rows:
  for pid,v in r['processes'].items():
   key=labels.get(pid,'loader');m=re.search(r'^VmRSS:\s+(\d+)',v['status'],re.M)
   if m:rss[key]=max(rss.get(key,0),int(m[1]))
  status=r.get('raft_status',[]);leaders={x['group']:x['commit'] for x in status if x['hosted'] and x['role'].lower()=='leader'}
  for x in status:
   if x['hosted'] and x['group'] in leaders:lag.append(dict(seconds=r['seconds'],node=x['node'],group=x['group'],commit_behind_cut=max(0,leaders[x['group']]-x['commit']),apply_behind_cut=max(0,leaders[x['group']]-x['applied'])))
  if 'space' in r:spaces.append(dict(seconds=r['seconds'],df=r['space']))
 out[tag]=dict(log_append_command_counts=commands,max_rss_mib={k:v/1024 for k,v in rss.items()},sampled_lag=lag,space_samples=spaces,
  note='Raft status calls are sequential every 300 seconds; lag is sampled against the observed leader cut, not a continuous bound. RSS is sampled every five seconds.')
(base/'balanced-resources.json').write_text(json.dumps(out,indent=2))
for tag,r in out.items():print(tag,r['max_rss_mib'], 'maximum sampled apply lag',max((v['apply_behind_cut'] for v in r['sampled_lag']),default=0))
