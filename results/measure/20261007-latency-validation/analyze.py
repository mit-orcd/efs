#!/usr/bin/env python3
"""Analyze matching histograms; summarize timed complete syscalls from full traces."""
import sys,pathlib,json,re,math,statistics,collections
root=pathlib.Path(sys.argv[1]);rows=json.loads((root/'results.json').read_text())
def fields(line):return dict(re.findall(r'(\w+)=([^\s]+)',line))
def bounds(i):
 if i<256:return i,i
 s=i//128-1;l=(128+i%128)<<s;return l,l+(1<<s)-1
def quantile(bins,n,pct=99):
 rank=(n*pct+99)//100;count=0
 for i,num in sorted(bins.items()):
  count+=num
  if count>=rank:return bounds(i)
 raise ValueError('histogram count mismatch')
summary=[]
for key in sorted({(r['variant'],r['root'],r['sync']) for r in rows if r['phase']=='baseline' and not r.get('excluded')}):
 rr=[r for r in rows if r['phase']=='baseline' and not r.get('excluded') and (r['variant'],r['root'],r['sync'])==key]
 hist=collections.Counter();samples=[];p99s=[];maximum=0; rates=[]
 for r in rr:
  assert r['valid'];m=r['metrics'][0];path=root/r['phase']/r['name']/(r['phase']+'.stdout')
  h=fields(next(l for l in path.read_text().splitlines() if l.startswith('BENCH_HIST ')))
  assert h['version']=='1';bins={int(k):int(v) for k,v in (x.split(':') for x in h['bins'].split(','))}
  n=int(h['n']);assert sum(bins.values())==n==int(m['ops'])==int(m['lat_samples'])
  lo,hi=quantile(bins,n);assert lo==int(m['p99_lower_us']) and min(hi,int(h['max_us']))==int(m['p99_us'])
  hist.update(bins);samples.append(n);p99s.append(int(m['p99_us']));maximum=max(maximum,int(m['max_us']))
  rates.append(float(m['ops_s'])*65536/(1<<30))
 lo,hi=quantile(hist,sum(samples));hi=min(hi,maximum)
 item=dict(variant=key[0],root=key[1],sync=key[2],repeats=len(rr),samples=sum(samples),min_samples=min(samples),GiB_s_mean=statistics.mean(rates),GiB_s_std=statistics.stdev(rates) if len(rates)>1 else None,GiB_s_min=min(rates),GiB_s_max=max(rates),p99_low_us=lo,p99_high_us=hi,per_run_p99_min=min(p99s),per_run_p99_max=max(p99s),max_us=maximum)
 summary.append(item)
 print(f'{key} n={len(rr)} GiB/s={item["GiB_s_mean"]:.3f} pooled p99={lo}-{hi}us runrange={min(p99s)}-{max(p99s)}us max={maximum}us samples={sum(samples)}')
(root/'summary.json').write_text(json.dumps(summary,indent=2))

def trace_summary(path):
 begin=end=None;pending={};calls=[];unparsed=[]
 for line in path.read_text(errors='replace').splitlines():
  match=re.match(r'^(\d+)\s+(\d+\.\d+)\s+(.*)',line)
  if not match:continue
  pid,ts,body=match.groups();ts=float(ts)
  if '"BENCH_PHASE begin\\n"' in body:begin=ts
  if '"BENCH_PHASE end\\n"' in body:end=ts
  first=re.match(r'(\w+)\(',body);resumed=re.match(r'<\.\.\. (\w+) resumed>',body)
  if '<unfinished ...>' in body:
   assert first,body
   pending[pid]=(ts,first.group(1),body);continue
  if resumed:
   prior=pending.pop(pid,None)
   if prior is None:unparsed.append(line);continue
   start,name,original=prior;assert name==resumed.group(1)
  elif first:start,name,original=ts,first.group(1),body
  else:continue
  duration=re.search(r'<(\d+\.\d+)>$',body)
  if not duration:
   unparsed.append(line);continue
  dt=float(duration.group(1));calls.append((start,start+dt,name,dt,original+' '+body))
 assert begin and end and end>begin,(path,begin,end)
 counts=collections.Counter();totals=collections.Counter();maxima=collections.Counter();errors=collections.Counter();crossing=collections.Counter()
 for start,finish,name,duration,body in calls:
  if start>=begin and finish<=end:
   counts[name]+=1;totals[name]+=duration;maxima[name]=max(maxima[name],duration)
   if '= -1 ' in body:errors[name]+=1
  elif start<end and finish>begin:crossing[name]+=1
 result=dict(begin=begin,end=end,wall_s=end-begin,complete_calls=sum(counts.values()),crossing_boundary_calls=dict(crossing),unparsed=len(unparsed),pending=len(pending),syscalls={name:dict(calls=counts[name],wall_sum_s=totals[name],mean_us=totals[name]*1e6/counts[name],max_us=maxima[name]*1e6,errors=errors[name]) for name in sorted(counts)})
 path.with_name('timed-summary.json').write_text(json.dumps(result,indent=2))
 return result
for r in rows:
 if r['phase']=='strace':
  p=root/'strace'/r['name']/'trace.txt'
  if p.exists():
   result=trace_summary(p)
   assert result['syscalls'].get('writev',{}).get('calls')==int(r['metrics'][0]['ops']),(r['name'],result)
   print('trace',r['name'],'writev',result['syscalls']['writev'],'unparsed',result['unparsed'])
