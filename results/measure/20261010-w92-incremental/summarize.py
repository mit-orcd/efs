import json,re,statistics
from pathlib import Path
out=Path(__file__).resolve().parent
summary={'live':[],'frozen':[]}
for r in range(1,4):
 p=out/f'round{r}-tiny.log'
 if not p.exists():continue
 matches=re.findall(r'6,000/6,000 files \(100.0%\), ([\d.]+) s, ([\d.]+) files/s',p.read_text())
 if not matches:continue
 nodes=[]
 for ip in (111,112,113):
  s=(out/f'node{ip}-round{r}-perf.txt').read_text()
  m=re.search(r'([\d.]+)%\s+[\d.]+%\s+\[.\] host_query_thread',s)
  cycles=re.search(r'Event count \(approx.\): (\d+)',s)
  assert 'Total Lost Samples: 0' in s
  nodes.append({'node':ip,'cache_worker_inclusive_percent':float(m[1]),'sampled_cycles_estimate':int(cycles[1])})
 summary['live'].append({'round':r,'seconds':float(matches[-1][0]),'files_per_second':float(matches[-1][1]),'nodes':nodes,'oracle':json.loads((out/f'round{r}-oracle.jsonl').read_text())})
p=out/'w92-incremental-repeat-frozen.jsonl'
if p.exists():
 rows=[json.loads(s) for s in p.read_text().splitlines()]
 for v in range(4):
  a=[s for s in rows if s['variant']==v];assert len(a)==3
  fresh=[s['builds'][0]['seconds'] for s in a];refresh=[b['seconds'] for s in a for b in s['builds'][1:]]
  summary['frozen'].append({'variant':v,'fresh_median_seconds':statistics.median(fresh),'refresh_median_seconds':statistics.median(refresh),'refresh_range_seconds':[min(refresh),max(refresh)],'process_cpu_median_seconds':statistics.median(s['cpu_user_seconds']+s['cpu_system_seconds'] for s in a),'process_peak_rss_range_kib':[min(s['process_peak_rss_kib'] for s in a),max(s['process_peak_rss_kib'] for s in a)],'builder_accounted_bytes':a[0]['builds'][0]['accounted_bytes'],'refresh_refolded':[b['refolded'] for s in a for b in s['builds'][1:]]})
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary,indent=2))
