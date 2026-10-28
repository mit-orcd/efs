#!/usr/bin/env python3
"""Summarize retained Task N JSON results; never turns a failed case into PASS."""
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence',type=Path)
a=p.parse_args()
rows=[]
for file in sorted(a.evidence.glob('*-report.json')):
    r=json.loads(file.read_text());c=r.get('checks',{});writers=r.get('writers',[])
    rows.append(dict(case=file.name.removesuffix('-report.json'),verdict=r.get('verdict'),
        integrity=r.get('integrity'),wall_s=r.get('wall_seconds'),events=len(r.get('events',[])),
        acknowledged=sum(w['acked'] for w in writers),failed=sum(w['failed'] for w in writers),
        writer_latency_s=[dict(writer=w['writer'],**w.get('write_latency_s',{})) for w in writers],
        fsync_max_s=c.get('final_fsync_max_s'),report_recv_timeouts=c.get('report_recv_timeouts'),
        report_max_ms=c.get('report_max_ms'),report_budget_exhausted=c.get('report_budget_exhausted'),
        accounting=c.get('accounting'),warm=c.get('warm_primary'),peer=c.get('warm_peer'),cold=c.get('cold'),
        cleanup_error=r.get('cleanup_error'),exception=r.get('exception')))
samples={}
for file in (a.evidence/'host-samples.jsonl',a.evidence/'followup-host-samples.jsonl'):
    if not file.exists():continue
    previous=None
    for line in file.read_text().splitlines():
        r=json.loads(line);v=list(map(int,r['cpu'].split()[1:9]))
        if previous and previous['stage']==r['stage']:
            old=list(map(int,previous['cpu'].split()[1:9]));delta=[x-y for x,y in zip(v,old)]
            if sum(delta)>0:
                samples.setdefault(r['stage'],[]).append(100*delta[4]/sum(delta))
        previous=r

io={}
for label,values in samples.items():
    values.sort();io[label]=dict(samples=len(values),mean_percent=sum(values)/len(values),
        p99_percent=values[min(len(values)-1,int(.99*len(values)))],max_percent=max(values))
print(json.dumps(dict(cases=rows,host_iowait=io),indent=2))
