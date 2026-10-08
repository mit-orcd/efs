#!/usr/bin/env python3
"""Validate the complete eight-case evidence and reproduce the summary table."""
import json
import gzip
import re
from pathlib import Path
import statistics

base=Path(__file__).resolve().parent
live=base/'live'
rows=json.loads((live/'results.json').read_text())
expected={f'{engine}-{mode}-r{repeat}' for engine in ('files','containers')
          for mode in ('buffered','direct') for repeat in (1,2)}
assert {row['name'] for row in rows}==expected and len(rows)==8
assert (live/'finished').read_text().strip()=='PASS'
assert json.loads((live/'installed-before.json').read_text())==json.loads((live/'installed-after.json').read_text())
commands=0
for row in rows:
    assert row['status']=='PASS'
    case=live/row['name']
    phases={phase['phase']:phase for phase in row['phases']}
    assert set(phases)=={'create','read','churn','verify','restart-verify','restart-delete'}
    for label,phase in phases.items():
        assert phase['errors']==0 and phase['workers']==4
        if label in ('read','churn'):
            assert phase['wall_s']>=30
        else:
            assert phase['operations']==256
        if label in ('read','verify','restart-verify'):
            assert phase['integrity']
        if label!='restart-delete':
            assert phase['bytes']==phase['operations']*(128*1024 if label=='churn' else 512*1024)
    for tag in ('start','restart'):
        nodes=json.loads((case/f'daemon-arguments-{tag}.json').read_text())
        assert len(nodes)==4
        for i,node in enumerate(nodes,1):
            argv=node['argv']
            assert argv[argv.index('--node-id')+1]==str(i)
            assert argv[argv.index('--port')+1]==str(27431+i)
            assert argv[argv.index('--addr')+1]=='127.0.0.1'
            assert argv[argv.index('--storage-engine')+1]==row['engine']
            assert argv[argv.index('--writers')+1]=='2'
            assert argv[argv.index('--quota')+1]=='2G'
            assert ('--direct-io' in argv)==(row['io']=='direct')
            assert ('--no-direct-io' in argv)==(row['io']=='buffered')
            assert ('--join' in argv)==(i>1)
            if i>1:assert argv[argv.index('--join')+1]=='127.0.0.1:27432'
            assert node['environment']==['EFS_MD_RAFT_N=4','EFS_TRANSPORT=tcp']
            assert Path(argv[argv.index('--storage')+1]).parts[-2:]==(row['name'],f'n{i}')
            commands+=1
    inventory=json.loads((case/'inventory-delete.json').read_text())
    assert len(inventory)==4 and all(all(v==0 for v in node.values()) for node in inventory)
    assert sum(node['data_records'] for node in json.loads((case/'inventory-live.json').read_text()))>0
for mode in ('buffered','direct'):
    flags=json.loads((live/f'containers-{mode}-r1/observed-container-fd-flags.json').read_text())
    assert len(flags)==4 and all(row['O_DIRECT']==(mode=='direct') for row in flags)
placement_lines=['| Case | File inode metadata group | Read MiB/s | Durable overwrite MiB/s | Delete files/s |','|---|---:|---:|---:|---:|']
case_summary=[]
for row in rows:
    case=live/row['name']
    inos=set()
    logs=list(case.glob('node*-start.log')) or list(case.glob('node*-start.log.gz'))
    assert len(logs)==4
    for log in logs:
        text=gzip.open(log,'rt').read() if log.suffix=='.gz' else log.read_text()
        for line in text.splitlines():
            if 'applied session' in line and (match:=re.search(r'ino=(\d+)',line)):
                inos.add(int(match[1]))
    assert len(inos)==256 and len({ino&1 for ino in inos})==1
    group=0 if next(iter(inos))&1 else 2
    phases={phase['phase']:phase for phase in row['phases']}
    placement_lines.append(f"| {row['name']} | {group} | {phases['read']['MiB_s']:.2f} | {phases['churn']['MiB_s']:.2f} | {phases['restart-delete']['ops_s']:.2f} |")
    leaders={}
    for tag in ('start','restart'):
        path=case/f'raft-status-{tag}.txt'
        text=path.read_text() if path.exists() else ''
        leaders[tag]={group:int(leader)+1 for group,leader in re.findall(
            r'group (\d+) hosted=1 role=LEADER leader=(\d+)',text)}
    case_summary.append({'name':row['name'],'file_inode_group':group,'observed_file_inodes':len(inos),'leaders_by_group':leaders,
                         'phases':row['phases']})
summary=[]
lines=['| Engine | I/O | Read MiB/s | Durable overwrite MiB/s | Delete files/s | Write p99 ms |',
       '|---|---|---:|---:|---:|---:|']
for engine in ('files','containers'):
    for mode in ('buffered','direct'):
        selected=[{v['phase']:v for v in row['phases']} for row in rows if row['engine']==engine and row['io']==mode]
        entry={'engine':engine,'io':mode,'repeats':2}
        values=[]
        for name,phase,metric in [('read','read','MiB_s'),('write','churn','MiB_s'),('delete','restart-delete','ops_s'),('write_p99','churn','p99_ms'),('create','create','MiB_s'),('read_p99','read','p99_ms')]:
            samples=[r[phase][metric] for r in selected]
            entry[name]={'min':min(samples),'max':max(samples),'median':statistics.median(samples),'samples':samples}
            if name in ('read','write','delete','write_p99'):
                values.append(f'{min(samples):.2f}–{max(samples):.2f}')
        lines.append('| '+engine+' | '+mode+' | '+' | '.join(values)+' |')
        summary.append(entry)
(base/'summary.json').write_text(json.dumps({'passed_cases':8,'verified_daemon_commands':commands,'summary':summary,'cases':case_summary},indent=2)+'\n')
(base/'placement-summary.md').write_text('\n'.join(placement_lines)+'\n')
(base/'summary.md').write_text('\n'.join(lines)+'\n\nRanges are min–max of two repeats, not confidence intervals.\n')
print(f'PASS: eight cases; {commands} daemon command checks; all checksum/restart/physical GC gates; installed processes unchanged')
print('\n'.join(lines))
