import pathlib,json,statistics,re
root=pathlib.Path('/tmp/efs-writer-study-results-final-20261007');result=json.loads((root/'results.json').read_text());out=[]
for row in result:
 vals=[x['metrics'][0] for x in row['repeats'] if x['returncode']==0 and x.get('metrics_valid')]
 if not vals:continue
 name=row['name'];stats={}
 p=root/name/'perf/perf.stdout'
 if p.exists():
  m=next((line for line in p.read_text().splitlines() if line.startswith('BENCH_WAIT ')),None)
  if m:
   fields={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',m)};stats=fields.copy()
   for field in ['admission','queue','service','resume','lock']:stats[field+'_avg_us']=fields[field+'_us']/max(1,fields['jobs'])
 item=dict(name=name,repeats=len(vals),mean_GiB_s=statistics.mean(float(x['GiB_s']) for x in vals),min_GiB_s=min(float(x['GiB_s']) for x in vals),max_GiB_s=max(float(x['GiB_s']) for x in vals),mean_p99_us=statistics.mean(float(x['p99_us']) for x in vals),max_latency_us=max(int(x['max_us']) for x in vals),writers=int(vals[0]['writers']),status=row['status'],wait=stats)
 out.append(item)
(root/'comparison.json').write_text(json.dumps(out,indent=2))
for label in ['root1','root2','roots2']:
 for direct in ['buffered','direct']:
  print(label,direct)
  for qd in [1,16,64]:
   for rw in ['write','read']:
    subset=[x for x in out if x['name'].startswith('data-'+direct+'-') and '-qd%d-%s-%s'%(qd,rw,label) in x['name']]
    print(qd,rw,'; '.join('%s %.3fGiB/s p99=%.0fus n=%d'%(x['writers'],x['mean_GiB_s'],x['mean_p99_us'],x['repeats']) for x in subset))
print('WAIT root1 buffered qd64')
for x in out:
 if '-buffered-' in x['name'] and '-qd64-write-root1' in x['name']:print(x['name'],x['wait'])
print('failed',sum(x['status']!='PASS' for x in result),'baseline_repeats',sum(len(x['repeats']) for x in result),'perf_runs',sum('perf' in x['runs'] for x in result))
