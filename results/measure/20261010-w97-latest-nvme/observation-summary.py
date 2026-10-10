"""Offline measured windows; do not infer cache occupancy from host writes."""
import hashlib,json,math
from pathlib import Path
base=Path(__file__).resolve().parent
cell=base/'continuous'
samples=[json.loads(x) for x in (cell/'samples.jsonl').read_text().splitlines()]
r=json.loads((cell/'continuous-analysis.json').read_text())
def window(lo,hi):
 a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds']
 def disks(s):return {v[2]:list(map(int,v[3:])) for v in map(str.split,s['diskstats'].splitlines())}
 old,new=disks(a),disks(b);d=[y-x for x,y in zip(old['nvme0n1'],new['nvme0n1'])]
 return dict(start=a['seconds'],end=b['seconds'],files_s=sum(y-x for x,y in zip(a['progress']['counts'],b['progress']['counts']))/dt,nvme_host_write_GiB=d[6]*512/2**30,nvme_flush_mean_ms=d[16]/d[15] if d[15] else None)
read=json.loads((cell/'read-latency.json').read_text());reads={}
for stage in sorted({v['stage'] for v in read}):
 values=sorted(v['us'] for v in read if v['stage']==stage)
 reads[stage]=dict(n=len(values),p50_us=values[math.ceil(.5*len(values))-1],p95_us=values[math.ceil(.95*len(values))-1],p99_us=values[math.ceil(.99*len(values))-1],max_us=values[-1])
origin=json.loads((cell/'timebase.json').read_text())['start_mono_us'];phases=r['phases'];drained=(phases['pause_drained_us']-origin)/1e6
pause_rows=[s for s in samples if drained<=s['seconds']<1800]
assert pause_rows and all(s['progress']['active_workers']==0 and s['progress']['paused'] for s in pause_rows)
assert len({tuple(s['progress']['counts']) for s in pause_rows})==1
provenance=json.loads((base/'source-provenance.json').read_text())
for name,digest in provenance['controller_sha256'].items():
 p=base/name
 assert hashlib.sha256(p.read_bytes()).hexdigest()==digest,name
for name in ('native','live','measurement','run','final'):
 assert (base/f'{name}-exit.txt').read_text().strip()=='0',name
result=dict(engine=provenance['engine_commit'],build_id=provenance['build_id'],one_minute_windows=[window(a,a+60) for a in (0,60,1140,1800,1860,2940)],writing_phases=[window(0,1200),window(1800,3000)],drain_seconds=(phases['pause_drained_us']-phases['pause_request_us'])/1e6,drained_pause_seconds=(phases['resume_us']-phases['pause_drained_us'])/1e6,paused_files=sum(pause_rows[0]['progress']['counts']),pause_counts_unchanged=True,total_files=sum(r['counts']),read_probes=reads,inbox_refusals={str(n):sum(w['nodes'][n-1]['counters']['inbox_drop'] for w in r['windows']) for n in range(1,5)},controller_hashes_match=True,root_smart_present=(base/'nvme-health.log').exists() and (base/'nvme-health.log').stat().st_size>0)
(base/'observation-summary.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
