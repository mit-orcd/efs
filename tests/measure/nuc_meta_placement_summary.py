#!/usr/bin/env python3
"""Reduce matched NUC metadata-placement windows without assuming completion."""
import argparse,json
from pathlib import Path
from nuc_disk_windows import windows
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args()
cells=[]
identity=json.loads((a.root/'identity.json').read_text())
role=identity.get('vary','metadata');assert role in ('metadata','data')
expected=['0-nvme','1-sata','2-nvme'] if role=='metadata' else ['0-sata','1-nvme','2-sata']
tags=identity.get('cells',expected);assert tags==expected,'wrong/duplicate placement cells'
for tag in tags:
    root=a.root/tag
    if not (root/'samples.jsonl').exists():continue
    samples=[json.loads(l) for l in (root/'samples.jsonl').read_text().splitlines()]
    progress=[json.loads(l) for l in (root/'progress.jsonl').read_text().splitlines()]
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
            counts={k:after['counters'][k]-before['counters'][k] for k in ('sync','sync_error','inbox_drop','drop_data','drop_empty','drop_other','flow_busy')}
            h={key:dict(n=after['hist'][key]['n']-before['hist'][key]['n'],
                sum=after['hist'][key]['sum']-before['hist'][key]['sum']) for key in ('sync_us','wal_entries','kv_release_us','applied_persist_us')}
            assert all(v>=0 for v in counts.values()) and all(v['n']>=0 and v['sum']>=0 for v in h.values()),'counter reset inside timed window'
            commands={k:after['commands'][k]-before['commands'][k] for k in after['commands']}
            assert all(v>=0 for v in commands.values()),'command counter reset inside timed window'
            begin=before['mono_us']/1e6-origin;finish=after['mono_us']/1e6-origin
            pbegin=min(progress,key=lambda r:abs(r['seconds']-begin));pend=min(progress,key=lambda r:abs(r['seconds']-finish))
            completed=sum(pend['counts'])-sum(pbegin['counts']);assert completed>=0
            nodes.append(dict(node=node,counters=counts,hist=h,commands=commands,observer_completed_files=completed,
                count_start_s=pbegin['seconds'],count_end_s=pend['seconds'],observer_start_s=before['mono_us']/1e6-origin,
                observer_end_s=after['mono_us']/1e6-origin))
        ranges[label]['nodes']=nodes
        sync_n=sum(v['hist']['sync_us']['n'] for v in nodes)
        ranges[label]['pooled_raft_sync_ms']=sum(v['hist']['sync_us']['sum'] for v in nodes)/sync_n/1000 if sync_n else None
        entries=sum(v['hist']['wal_entries']['sum'] for v in nodes)
        for metric,hist_key,part in (('syncs','sync_us','n'),('entries','wal_entries','sum')):
            ranges[label]['replicated_raft_'+metric+'_per_completed_file']=sum(node['hist'][hist_key][part]/node['observer_completed_files'] for node in nodes) if nodes and all(node['observer_completed_files']>0 for node in nodes) else None
        names={'0':'empty','1':'create','5':'prepare','6':'decide','7':'resolve','9':'session','36':'close_step','41':'report_publish'}
        commands={}
        for node in nodes:
            for opcode,count in node['commands'].items():
                if count:commands[opcode]=commands.get(opcode,0)+count
        observed_files=sum(node['observer_completed_files'] for node in nodes)
        ranges[label]['replicated_new_log_commands']={names.get(k,'opcode_'+k):dict(entries=v,per_replica_completed_file_estimate=v/observed_files if observed_files else None) for k,v in commands.items()}
        ranges[label]['command_count_limits']='New append requests across replicas, including background and uncommitted work; not successful logical operation counts. Ratios use nearest one-second completed-file observations at each node observer boundary, rather than a mismatched common 30-second denominator.'
        ranges[label]['raft_entries_per_sync']=entries/sync_n if sync_n else None
        for phase in ('kv_release_us','applied_persist_us'):
            count=sum(v['hist'][phase]['n'] for v in nodes)
            ranges[label]['pooled_'+phase.replace('_us','_ms')]=sum(v['hist'][phase]['sum'] for v in nodes)/count/1000 if count else None
    disk_hosts=[dict(monotonic=r['seconds'],diskstats=r['diskstats']) for r in samples]
    physical={dev:windows(disk_hosts,dev,end) for dev in ('sda','nvme0n1')} if duration>=60 else {}
    accepted=json.loads((root/'complete.json').read_text()) if (root/'complete.json').exists() else None
    cells.append(dict(cell=tag,accepted=accepted,windows=ranges,physical=physical))
complete=len(cells)==3 and all(r['accepted'] for r in cells) and all((a.root/n).exists() and (a.root/n).read_text().strip()=='0' for n in ('tests-exit.txt','restore-exit.txt'))
report=dict(matrix_accepted=complete,vary=identity.get('vary','metadata'),cells=cells,limits='Fresh three-voter clusters and fixed engine. Metadata matrix fixes NVMe data; data matrix fixes SATA metadata. First/last completed approximately 30-second windows; cumulative non-atomic Raft histograms have sampling boundary skew. Sync/entry counts sum three replicas and include background work; they are not logical operation counts. Host physical block counters include queue time and other I/O. W52 focused compilation/testing overlaps the first metadata-matrix NVMe middle, not an uninstrumented exclusive-host ceiling. Cold checks sample 64 payloads per cell, not every file or power loss.')
(a.root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
for cell in cells:
    print(cell['cell'],'accepted',bool(cell['accepted']),{k:dict(files_s=round(v['files_s'],2),sync_ms=v['pooled_raft_sync_ms']) for k,v in cell['windows'].items()})
