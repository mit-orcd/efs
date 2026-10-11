#!/usr/bin/env python3
"""Reduce matched NUC metadata-placement windows without assuming completion."""
import argparse,json
from pathlib import Path
from nuc_disk_windows import windows
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args()
cells=[]
for tag in ('0-nvme','1-sata','2-nvme'):
    root=a.root/tag
    if not (root/'samples.jsonl').exists():continue
    samples=[json.loads(l) for l in (root/'samples.jsonl').read_text().splitlines()]
    origin=json.loads((root/'load-start.json').read_text())['mono_us']/1e6
    duration=samples[-1]['seconds']
    end=min(600,duration)
    ranges={}
    for label,start,stop in (('first',0,30),('last',max(0,end-30),end)):
        lo=min(samples,key=lambda r:abs(r['seconds']-start));hi=min(samples,key=lambda r:abs(r['seconds']-stop))
        seconds=hi['seconds']-lo['seconds']
        if seconds<20:continue
        ranges[label]=dict(start_s=lo['seconds'],end_s=hi['seconds'],
            files_s=(sum(hi['progress']['counts'])-sum(lo['progress']['counts']))/seconds,
            group_files_s={str(g):(hi['progress']['groups'][str(g)]-lo['progress']['groups'][str(g)])/seconds for g in (0,2)})
        nodes=[]
        for node in (1,2,3):
            path=root/f'n{node}-observations.jsonl'
            if not path.exists():continue
            rows=[json.loads(l) for l in path.read_text().splitlines()]
            before=min(rows,key=lambda r:abs(r['mono_us']/1e6-origin-lo['seconds']))
            after=min(rows,key=lambda r:abs(r['mono_us']/1e6-origin-hi['seconds']))
            counts={k:after['counters'][k]-before['counters'][k] for k in ('sync','sync_error','inbox_drop','flow_busy')}
            h={key:dict(n=after['hist'][key]['n']-before['hist'][key]['n'],
                sum=after['hist'][key]['sum']-before['hist'][key]['sum']) for key in ('sync_us','wal_entries','kv_release_us','applied_persist_us')}
            assert all(v>=0 for v in counts.values()) and all(v['n']>=0 and v['sum']>=0 for v in h.values()),'counter reset inside timed window'
            nodes.append(dict(node=node,counters=counts,hist=h,observer_start_s=before['mono_us']/1e6-origin,
                observer_end_s=after['mono_us']/1e6-origin))
        ranges[label]['nodes']=nodes
        sync_n=sum(v['hist']['sync_us']['n'] for v in nodes)
        ranges[label]['pooled_raft_sync_ms']=sum(v['hist']['sync_us']['sum'] for v in nodes)/sync_n/1000 if sync_n else None
    disk_hosts=[dict(monotonic=r['seconds'],diskstats=r['diskstats']) for r in samples]
    physical={dev:windows(disk_hosts,dev,end) for dev in ('sda','nvme0n1')} if duration>=60 else {}
    accepted=json.loads((root/'complete.json').read_text()) if (root/'complete.json').exists() else None
    cells.append(dict(cell=tag,accepted=accepted,windows=ranges,physical=physical))
complete=len(cells)==3 and all(r['accepted'] for r in cells) and all((a.root/n).exists() and (a.root/n).read_text().strip()=='0' for n in ('tests-exit.txt','restore-exit.txt'))
report=dict(matrix_accepted=complete,cells=cells,limits='Fresh three-voter clusters, fixed engine and NVMe data, changing metadata device. First/last completed approximately 30-second windows; cumulative non-atomic Raft histograms have sampling boundary skew. Host physical block counters include queue time and other I/O. W52 focused compilation/testing overlaps the first NVMe middle, not an uninstrumented exclusive-host ceiling. Cold checks sample 64 payloads per cell, not every file or power loss.')
(a.root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
for cell in cells:
    print(cell['cell'],'accepted',bool(cell['accepted']),{k:dict(files_s=round(v['files_s'],2),sync_ms=v['pooled_raft_sync_ms']) for k,v in cell['windows'].items()})
