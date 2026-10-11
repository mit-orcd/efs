#!/usr/bin/env python3
"""Reduce retained NUC allocation controls; never treat missing cells as passes."""
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
a=p.parse_args();cells=[]
def disk(raw,name):
    for line in raw.splitlines():
        parts=line.split()
        if parts[2]==name:return list(map(int,parts[3:]))
    raise ValueError('device absent: '+name)
for drive,device in [('nvme','nvme0n1'),('sata','sda')]:
    for index,mode in enumerate(('grow','ring','ring','grow')):
        name=f'w97-{drive}-{index}-{mode}';folder=a.root/name
        final=json.loads((folder/'final.json').read_text())
        assert final['verified'] and (a.root/(name+'-exit.txt')).read_text().strip()=='0'
        first=[];last=[]
        for writer in range(3):
            windows=[json.loads(s) for s in (folder/f'writer{writer}.jsonl').read_text().splitlines()]
            assert len(windows)>=14
            first.append(windows[0]);last.append(windows[-1])
        hosts=[json.loads(s) for s in (folder/'host.jsonl').read_text().splitlines()]
        before=disk(hosts[0]['diskstats'],device);after=disk(hosts[-1]['diskstats'],device)
        delta=[b-a for a,b in zip(before,after)]
        # Linux diskstats milliseconds are sums across requests, not latency
        # percentiles. Device mapper/host activity may share this physical disk.
        row=dict(name=name,drive=drive,mode=mode,mean_operations_s=final['total_operations_s'],
                 first_window_operations_s=sum(w['operations_s'] for w in first),
                 last_window_operations_s=sum(w['operations_s'] for w in last),
                 first_window_sync_ms=[w['sync_mean_ms'] for w in first],
                 last_window_sync_ms=[w['sync_mean_ms'] for w in last],
                 writer_p99_us=[w['sync_p99_us'] for w in final['writers']],
                 cpu_user_seconds=final['cpu_user_seconds'],cpu_system_seconds=final['cpu_system_seconds'],
                 allocated_bytes=sum(w['allocated_bytes'] for w in final['writers']),
                 rotations=sum(w['rotations'] for w in final['writers']),
                 rotation_seconds=sum(w['rotation_seconds'] for w in final['writers']),
                 physical_write_requests=delta[4],physical_write_mean_ms=delta[7]/delta[4] if delta[4] else None,
                 physical_flush_requests=delta[15] if len(delta)>15 else None,
                 physical_flush_mean_ms=delta[16]/delta[15] if len(delta)>16 and delta[15] else None)
        row['last_first_ratio']=row['last_window_operations_s']/row['first_window_operations_s']
        cells.append(row)
result=dict(cells=cells,limits='Eight-minute file controls; final complete 30-second window, not final partial window. Physical counters include other host I/O. No hardware ceiling or NAND-cache inference.')
(a.root/'allocation-summary.json').write_text(json.dumps(result,indent=2)+'\n')
for r in cells:print(r['name'],round(r['mean_operations_s'],2),round(r['first_window_operations_s'],2),round(r['last_window_operations_s'],2),round(r['last_first_ratio'],3))
