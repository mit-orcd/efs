"""Offline matched three-node device comparisons, with per-window sync observations."""
import json
from pathlib import Path
base=Path(__file__).resolve().parent;result={}
for tag,device in [('nvme','nvme0n1'),('sata','sda')]:
 cell=base/tag;samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()];origin=json.loads((cell/'load-start.json').read_text())['mono_us'];final=json.loads((cell/'load-final.json').read_text());obs={n:[json.loads(x) for x in (cell/f'n{n}-observations.jsonl').read_text().splitlines()] for n in range(1,4)}
 def window(lo,hi):
  a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds']
  def ds(s):return {v[2]:list(map(int,v[3:])) for v in map(str.split,s['diskstats'].splitlines())}
  old,new=ds(a),ds(b);d=[y-x for x,y in zip(old[device],new[device])];ios=d[0]+d[4];nodes=[]
  for n,rows in obs.items():
   x=max((r for r in rows if r['mono_us']<=origin+a['seconds']*1e6),key=lambda r:r['mono_us']);y=max((r for r in rows if r['mono_us']<=origin+b['seconds']*1e6),key=lambda r:r['mono_us']);hist={}
   for k,v in x['hist'].items():
    count=y['hist'][k]['n']-v['n'];total=y['hist'][k]['sum']-v['sum'];hist[k]=dict(n=count,sum=total,mean=total/count if count else None,bins=[b-a for a,b in zip(v['bins'],y['hist'][k]['bins'])])
   nodes.append(dict(node=n,start_s=(x['mono_us']-origin)/1e6,end_s=(y['mono_us']-origin)/1e6,hist=hist,counters={k:y['counters'][k]-v for k,v in x['counters'].items()}))
  counts=[y-x for x,y in zip(a['progress']['counts'],b['progress']['counts'])]
  return dict(start=a['seconds'],end=b['seconds'],files_s=sum(counts)/dt,group0_files_s=sum(counts[:4])/dt,group2_files_s=sum(counts[4:])/dt,device=dict(host_write_GiB=d[6]*512/2**30,write_MiB_s=d[6]*512/dt/2**20,mean_request_ms=(d[3]+d[7])/ios if ios else None,mean_flush_ms=d[16]/d[15] if len(d)>16 and d[15] else None,flush_s=d[15]/dt if len(d)>16 else None,busy_percent=d[9]/dt/10),nodes=nodes)
 assert final['errors']==[] and max(final['counts'])<512000
 result[tag]=dict(minutes=[window(i,i+60) for i in range(0,1200,60)],five_minutes=[window(i,i+300) for i in range(0,1200,300)],whole=window(0,1200),latency=final['latency'],files=sum(final['counts']),cold=json.loads((cell/'cold-verification.json').read_text()),catch_up=json.loads((cell/'catch-up.json').read_text()))
(base/'analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps({k:dict(files=v['files'],rate=v['whole']['files_s'],minutes=[round(w['files_s'],2) for w in v['minutes']],five_minute_rates=[round(w['files_s'],2) for w in v['five_minutes']],first_sync_ms=[w['hist']['sync_us']['mean']/1000 for w in v['minutes'][0]['nodes']],last_sync_ms=[w['hist']['sync_us']['mean']/1000 for w in v['minutes'][-1]['nodes']],latency=v['latency']) for k,v in result.items()},indent=2))
