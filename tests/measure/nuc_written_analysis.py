#!/usr/bin/env python3
"""Require all written-ring cells and reduce early/late block-counter windows."""
import argparse,json
from pathlib import Path
from nuc_disk_windows import windows
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args();rows=[]
for i,drive in enumerate(('nvme','sata','sata','nvme')):
    name=f'written-{i}-{drive}';folder=a.root/name
    if not folder.exists():
        name=f'{a.root.name}-{i}-{drive}';folder=a.root/name
    assert (a.root/(name+'-exit.txt')).read_text().strip()=='0'
    data=json.loads((folder/'final.json').read_text());assert data['verified'] and data['mode']=='written-ring'
    first=[];last=[]
    for writer in range(3):
        intervals=[json.loads(s) for s in (folder/f'writer{writer}.jsonl').read_text().splitlines()]
        assert len(intervals)>=max(1,data['seconds']//30-2);first.append(intervals[0]);last.append(intervals[-1])
    hosts=[json.loads(s) for s in (folder/'host.jsonl').read_text().splitlines()]
    row=dict(name=name,drive=drive,mean_operations_s=data['total_operations_s'],
             first_operations_s=sum(w['operations_s'] for w in first),late_operations_s=sum(w['operations_s'] for w in last),
             first_sync_ms=[w['sync_mean_ms'] for w in first],late_sync_ms=[w['sync_mean_ms'] for w in last],
             writer_p99_us=[w['sync_p99_us'] for w in data['writers']],
             cpu_seconds=data['cpu_user_seconds']+data['cpu_system_seconds'],
             reopened_exact_blocks=sum(w['reopened_exact_blocks'] for w in data['writers']),
             physical_windows=windows(hosts,'nvme0n1' if drive=='nvme' else 'sda',last[0]['seconds']))
    rows.append(row)
    print(name,round(row['mean_operations_s'],2),round(row['first_operations_s'],2),round(row['late_operations_s'],2),row['physical_windows'])
(a.root/'written-summary.json').write_text(json.dumps(dict(cells=rows,limits='First/late complete 30s writer windows; block snapshots approximate their edges within ~5s. Block averages include queue time/other host I/O, not device-only NAND service.'),indent=2)+'\n')
