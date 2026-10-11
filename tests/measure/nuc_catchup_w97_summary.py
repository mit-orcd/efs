#!/usr/bin/env python3
"""Reduce matched NUC metadata-placement windows without assuming completion."""
import argparse,json
from pathlib import Path
from nuc_disk_windows import disk
import statistics,re
def windows(hosts,device,last_seconds):
 result={}
 for label,start,end in (("first",0,60),("late",540,600)):
  a=min(hosts,key=lambda h:abs(h["monotonic"]-start));b=min(hosts,key=lambda h:abs(h["monotonic"]-end))
  dt=b["monotonic"]-a["monotonic"];assert 55<=dt<=65
  delta=[v-u for u,v in zip(disk(a["diskstats"],device),disk(b["diskstats"],device))]
  assert all(v>=0 for i,v in enumerate(delta) if i!=8)
  result[label]=dict(seconds=dt,write_requests=delta[4],write_mean_ms=delta[7]/delta[4] if delta[4] else None,host_write_MiB=delta[6]*512/2**20,flush_requests=delta[15] if len(delta)>15 else None,flush_mean_ms=delta[16]/delta[15] if len(delta)>16 and delta[15] else None)
 return result
p=argparse.ArgumentParser(description=__doc__);p.add_argument('root',type=Path);a=p.parse_args()
cells=[]
comparison=json.loads((a.root/'comparison.json').read_text())
tags=[f'{medium}-{i}-{variant}' for medium in ('sata','nvme') for i,variant in enumerate(('base','root','root','base'))]
for tag in tags:
    root=a.root/tag
    if not (root/'samples.jsonl').exists():continue
    samples=[json.loads(l) for l in (root/'samples.jsonl').read_text().splitlines()]
    progress=[json.loads(l) for l in (root/'progress.jsonl').read_text().splitlines()]
    origin=json.loads((root/'load-start.json').read_text())['mono_us']/1e6
    duration=samples[-1]['seconds']
    end=min(600,duration)
    ranges={}
    for label,start,stop in (('first',0,60),('last',540,600)):
        lo=min(samples,key=lambda r:abs(r['seconds']-start));hi=min(samples,key=lambda r:abs(r['seconds']-stop))
        seconds=hi['seconds']-lo['seconds']
        if seconds<55 or duration<600:continue
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
    physical={dev:windows(disk_hosts,dev,end) for dev in ('sda','nvme0n1')} if duration>=600 else {}
    accepted=json.loads((root/'complete.json').read_text()) if (root/'complete.json').exists() else None
    cells.append(dict(cell=tag,accepted=accepted,windows=ranges,physical=physical))
complete=len(cells)==8 and all(r['accepted'] for r in cells) and all((a.root/n).exists() and (a.root/n).read_text().strip()=='0' for n in ('tests-exit.txt','restore-exit.txt'))
effect={}
for medium in ('sata','nvme'):
 variants={v:[c for c in cells if c['cell'].startswith(medium+'-') and c['cell'].endswith('-'+v) and c['accepted'] and set(c['windows'])=={'first','last'}] for v in ('base','root')}
 if not all(len(v)==2 for v in variants.values()):continue
 metric={v:dict(early_files_s=[c['windows']['first']['files_s'] for c in cs],late_files_s=[c['windows']['last']['files_s'] for c in cs],fractional_drop=[1-c['windows']['last']['files_s']/c['windows']['first']['files_s'] for c in cs]) for v,cs in variants.items()}
 early={v:statistics.mean(x['early_files_s']) for v,x in metric.items()};late={v:statistics.mean(x['late_files_s']) for v,x in metric.items()};drop={v:statistics.mean(x['fractional_drop']) for v,x in metric.items()}
 effect[medium]=dict(repeats=metric,mean_early_files_s=early,mean_late_files_s=late,mean_fractional_drop=drop,change_in_drop_percentage_points=100*(drop['base']-drop['root']),late_throughput_change_percent=100*(late['root']/late['base']-1),limits='Two repeats per variant; descriptive comparison, not statistical proof or a hardware ceiling.')
report=dict(matrix_accepted=complete,comparison=comparison,cells=cells,effect=effect,limits='Identical W52 core, two W97 root-scope engine files differ. Three voters, metadata SATA, node1 leads both groups and remains stable throughout each timed cell. First/last unprofiled 60-second windows exclude the 450-second perf/strace window. Observer histograms are cumulative and non-atomic with sampling skew; physical block counters include all host I/O and queue time. Cold checks sample 64 exact files per cell, not every file or power loss. No assertion of cache, NAND or hardware ceiling.')
(a.root/'summary.json').write_text(json.dumps(report,indent=2)+'\n')
for cell in cells:
    print(cell['cell'],'accepted',bool(cell['accepted']),{k:dict(files_s=round(v['files_s'],2),sync_ms=v['pooled_raft_sync_ms']) for k,v in cell['windows'].items()})
