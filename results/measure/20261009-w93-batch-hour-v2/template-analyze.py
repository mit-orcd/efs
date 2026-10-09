import json,math
from pathlib import Path
base=Path(__file__).resolve().parent
start=json.loads((base/'timebase.json').read_text())['start_mono_us']
def histogram(a,b):
    bins=[y-x for x,y in zip(a['bins'],b['bins'])]
    n=b['n']-a['n'];s=b['sum']-a['sum']
    # Snapshot fields are independently cumulative atomics: use bin totals
    # for quantiles and report discrepancies rather than silently repairing n.
    total=sum(bins)
    def bound(q):
        target=math.ceil(q*total);seen=0
        if not total:return None
        for i,c in enumerate(bins):
            seen+=c
            if seen>=target:return 0 if i==0 else (2**(i-1) if i<len(bins)-1 else 'overflow')
    return dict(n=n,sum=s,mean=s/n if n else None,bins=bins,bin_count_delta=total-n,p50_upper=bound(.5),p95_upper=bound(.95),p99_upper=bound(.99),zero_percent=100*bins[0]/total if total else None,one_percent=100*bins[1]/total if total else None,cumulative_max=b['max'])
summary=[]
for node in range(1,5):
    rows=[json.loads((base/f'baseline-n{node}.json').read_text())]+[json.loads(l) for l in (base/f'n{node}-observations.jsonl').read_text().splitlines()]
    for label,lo,hi in [('all',0,240),('early',0,80),('middle',80,160),('late',160,240)]:
        a=max((r for r in rows if r['mono_us']<=start+lo*1000000),key=lambda r:r['mono_us'])
        b=max((r for r in rows if r['mono_us']<=start+hi*1000000),key=lambda r:r['mono_us'])
        out=dict(node=node,window=label,start_seconds=(a['mono_us']-start)/1000000,end_seconds=(b['mono_us']-start)/1000000,counters={k:b['counters'][k]-a['counters'][k] for k in a['counters']},hist={k:histogram(a['hist'][k],b['hist'][k]) for k in a['hist']})
        out['follow_reject']={k:b['follow_reject'][k]-a['follow_reject'][k] for k in a['follow_reject']}
        summary.append(out)
(base/'analysis.json').write_text(json.dumps(summary,indent=2))
for r in summary:
    if r['window']=='all':
        print('node',r['node'],'counters',r['counters'])
        for k,h in r['hist'].items():print(k,'n',h['n'],'mean',h['mean'],'p50<=',h['p50_upper'],'p95<=',h['p95_upper'],'one%',h['one_percent'],'bin discrepancy',h['bin_count_delta'])
samples=[json.loads(l) for l in (base/'samples.jsonl').read_text().splitlines()]
windows=[]
for lo,hi in [(0,80),(80,160),(160,240)]:
    a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds']
    windows.append(dict(start=a['seconds'],end=b['seconds'],files_per_second=(b['progress']['created']-a['progress']['created'])/dt))
(base/'rates.json').write_text(json.dumps(windows,indent=2));print('rates',windows)
