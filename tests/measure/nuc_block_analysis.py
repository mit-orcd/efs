#!/usr/bin/env python3
"""Pair NUC physical issue/completion samples; report ambiguity and trace edges."""
import argparse,collections,gzip,json,re,statistics
from pathlib import Path
row=re.compile(r'(\d+\.\d+):\s+block:block_rq_(issue|complete):\s+(\d+),(\d+)\s+(\S+)\s+.*?\s+(\d+) \+ (\d+)')
def analyze(path):
 pending=collections.defaultdict(collections.deque);values=collections.defaultdict(list)
 counters=collections.Counter();errors=0;lost=0;duration=[]
 opener=gzip.open if str(path).endswith('.gz') else open
 with opener(path,'rt') as source:
  for line in source:
   if 'LOST' in line:lost+=1
   m=row.search(line)
   if not m:continue
   stamp,kind,major,minor,flags,sector,length=m.groups();stamp=float(stamp)
   duration.append(stamp);key=(major,minor,sector,length);counters[kind]+=1
   if kind=='issue':
    item=[stamp,flags,int(length),False]
    if pending[key]:
     counters['ambiguous_issues']+=1;item[3]=True
     for active in pending[key]:active[3]=True
    pending[key].append(item)
   elif not pending[key]:counters['unmatched_completions']+=1
   else:
    began,issued_flags,sectors,ambiguous=pending[key].popleft()
    error=re.search(r'\[(-?\d+)\]\s*$',line)
    if error and int(error[1]):errors+=1;continue
    if ambiguous:counters['ambiguous_completions']+=1;continue
    if stamp<began:counters['negative_pairs']+=1;continue
    values[issued_flags].append((stamp-began)*1000)
 counters['unmatched_issues']=sum(map(len,pending.values()))
 stats={}
 for flags,latencies in values.items():
  latencies.sort();n=len(latencies)
  stats[flags]=dict(count=n,mean_ms=statistics.mean(latencies),
   p50_ms=latencies[(n-1)//2],p95_ms=latencies[min(n-1,int(.95*n))],
   p99_ms=latencies[min(n-1,int(.99*n))],max_ms=latencies[-1])
 return dict(path=str(path),counters=dict(counters),error_completions=errors,lost_record_lines=lost,
  event_span_s=max(duration)-min(duration) if duration else 0,by_issued_rwbs=stats,
  limits='Physical issue-to-completion, not submission queue latency or NAND-only time. Sector/length FIFO pairs omit overlapping indistinguishable keys; unmatched edges and partial completions are reported. Trace windows are instrumented controls, not untraced speed acceptance.')
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args()
 results=[analyze(path) for path in sorted(a.root.glob('*/*-events.txt*'))]
 assert results,'no event traces'
 (a.root/'block-summary.json').write_text(json.dumps(results,indent=2)+'\n')
 for result in results:print(result['path'],result['by_issued_rwbs'],result['counters'])
if __name__=='__main__':main()
