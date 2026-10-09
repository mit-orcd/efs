"""Analyze tracepoint syscall cohorts and verify WAL record headers on disk."""
import collections,gzip,json,re,statistics
from pathlib import Path

base=Path(__file__).resolve().parent
identity=json.loads((base/'identity.json').read_text())
servers={int(s['pid']):dict(s,node=int(n)) for n,s in identity['servers'].items()}
headers=json.loads((base/'wal-headers.json').read_text())
def dist(a):
    a=sorted(a)
    return dict(n=len(a),mean=statistics.mean(a) if a else None,p50=a[int((len(a)-1)*.5)] if a else None,p95=a[int((len(a)-1)*.95)] if a else None,p99=a[int((len(a)-1)*.99)] if a else None,max=max(a) if a else None)

partial={};timeline=[];syncs=[];errors=collections.Counter();diagnostics=collections.Counter();first=None;last=None;all_fsync=[];commands=collections.defaultdict(collections.Counter)
with gzip.open(base/'perf-script.txt.gz','rt') as f:
    for line in f:
        m=re.search(r'\s(\d+)/(\d+)\s+(\d+\.\d+):\s+syscalls:sys_(enter|exit)_(\w+):\s+(.*)',line)
        if not m:
            if 'LOST' in line.upper():diagnostics['lost_lines']+=1
            else:diagnostics['unparsed_lines']+=1
            continue
        pid,tid,stamp=int(m[1]),int(m[2]),float(m[3]);kind,name,body=m[4],m[5],m[6]
        if pid not in servers:diagnostics['unknown_pid']+=1;continue
        first=stamp if first is None else min(first,stamp);last=stamp
        key=(pid,tid,name)
        if kind=='enter':
            fd=int(re.search(r'fd: (0x[\da-f]+)',body)[1],16)
            ev=dict(pid=pid,tid=tid,start=stamp,fd=fd,name=name)
            if name=='pwrite64':
                ev.update(count=int(re.search(r'count: (0x[\da-f]+)',body)[1],16),offset=int(re.search(r'pos: (0x[\da-f]+)',body)[1],16))
            partial[key]=ev;continue
        if key not in partial:
            # Exit pwrite events are unfiltered: fragment writes intentionally
            # have no selected entry and are ignored, as are attach boundaries.
            diagnostics['unmatched_exit_'+name]+=1;continue
        ev=partial.pop(key);ev['end']=stamp;ev['rc']=int(body.split()[0],16)
        if ev['rc']>=2**63:ev['rc']-=2**64
        if ev['rc']<0:errors[name]+=1;continue
        ev['duration_ms']=(stamp-ev['start'])*1000
        if name=='fsync':all_fsync.append(ev)
        s=servers[pid]
        if ev['fd']!=s['fd']:continue
        if name=='pwrite64':
            if ev['rc']!=ev['count']:diagnostics['short_wal_write']+=1;continue
            h=bytes.fromhex(headers[str(s['node'])].get(str(ev['offset']),''))
            if len(h)<13 or int.from_bytes(h[4:8],'big')+8!=ev['count']:
                diagnostics['bad_record_header']+=1;continue
            ev.update(type=h[8],group=int.from_bytes(h[9:13],'big'))
            if h[8]==2 and len(h)>=34:
                tag=str(h[33])+(':'+str(h[34]) if h[33]==9 and len(h)>=35 else '')
                commands[s['node']][tag]+=1
            timeline.append((ev['end'],0,'write',ev))
        elif name=='fdatasync':
            timeline.append((ev['start'],1,'sync',ev));syncs.append(ev)
timeline.sort(key=lambda e:(e[0],e[1]))
pending=collections.defaultdict(list);cohorts=[];seen=set()
for _,_,kind,ev in timeline:
    pid=ev['pid']
    if kind=='write':pending[pid].append(ev);continue
    batch=pending[pid];pending[pid]=[]
    if pid not in seen:seen.add(pid);continue
    cohorts.append(dict(node=servers[pid]['node'],seconds=ev['start']-first,duration_ms=ev['duration_ms'],entries=sum(e['type']==2 for e in batch),records=len(batch),bytes=sum(e['count'] for e in batch),groups=sorted(set(e['group'] for e in batch))))
summaries=[]
for node in range(1,5):
    for label,lo,hi in [('all',0,301),('early',0,100),('middle',100,200),('late',200,301)]:
        c=[c for c in cohorts if c['node']==node and lo<=c['seconds']<hi]
        entries=sum(c['entries'] for c in c)
        summaries.append(dict(node=node,window=label,barriers=len(c),entries=entries,entries_per_sync=dist([c['entries'] for c in c]),bytes_per_sync=dist([c['bytes'] for c in c]),sync_ms=dist([c['duration_ms'] for c in c]),single_percent=100*sum(c['entries']==1 for c in c)/len(c) if c else None,zero_entry_barriers=sum(c['entries']==0 for c in c),multi_group_barriers=sum(len(c['groups'])>1 for c in c),approx_call_reduction_percent=100*(1-len(c)/entries) if entries else None))
with gzip.open(base/'cohorts.jsonl.gz','wt') as f:
    for c in cohorts:f.write(json.dumps(c)+'\n')
(base/'analysis.json').write_text(json.dumps(dict(first_timestamp=first,last_timestamp=last,diagnostics=diagnostics,errors=errors,unmatched_entries=len(partial),summaries=summaries,commands=commands,all_fsync_ms={str(n):dist([e['duration_ms'] for e in all_fsync if servers[e['pid']]['node']==n]) for n in range(1,5)}),indent=2))
for s in summaries:print(json.dumps(s))
print('diagnostics',dict(diagnostics),'errors',dict(errors),'unmatched entries',len(partial))

samples=[json.loads(l) for l in (base/'samples.jsonl').read_text().splitlines()]
def disks(s):return {p[2]:list(map(int,p[3:])) for line in s['diskstats'].splitlines() if len(p:=line.split())>=14}
windows=[]
for lo,hi in [(0,100),(100,200),(200,300)]:
    a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi));dt=b['seconds']-a['seconds']
    w=dict(start=a['seconds'],end=b['seconds'],files_per_second=(b['progress']['created']-a['progress']['created'])/dt,disks={})
    da,db=disks(a),disks(b)
    for dev in ['sda','nvme0n1']:
        d=[y-x for x,y in zip(da[dev],db[dev])]
        w['disks'][dev]=dict(busy_percent=d[9]/dt/10,write_iops=d[4]/dt,write_mib_s=d[6]*512/dt/1048576,write_await_ms=d[7]/d[4] if d[4] else None,flush_await_ms=d[16]/d[15] if len(d)>16 and d[15] else None)
    windows.append(w)
(base/'windows.json').write_text(json.dumps(windows,indent=2))
print(json.dumps(windows,indent=2))
