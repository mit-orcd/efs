"""Reconstruct successful WAL record writes assigned to subsequent sync starts.

These are syscall-boundary cohorts, not internal durable watermark telemetry.
Writes completed during a sync are conservatively assigned to the next sync.
Drop the first barrier at attach; exclude incomplete/failed syscalls.
"""
import collections, json, re, statistics
from pathlib import Path

base = Path(__file__).resolve().parent
def quantile(a, q):
    return sorted(a)[min(len(a)-1, int((len(a)-1)*q))] if a else None
def metrics(a):
    return dict(n=len(a),mean=statistics.mean(a) if a else None,p50=quantile(a,.5),p95=quantile(a,.95),p99=quantile(a,.99),max=max(a) if a else None)

def trace(path):
    partial={};events=[];unparsed=[]
    for line in path.read_text().splitlines():
        m=re.match(r'(\d+)\s+(\d+\.\d+)\s+(.*)',line)
        if not m:continue
        tid,stamp,body=int(m[1]),float(m[2]),m[3]
        if body.startswith(('+++','---')):continue
        if '<unfinished ...>' in body:
            partial[tid]=(stamp,body.replace('<unfinished ...>',''));continue
        if body.startswith('<... '):
            if tid not in partial:continue
            began,prefix=partial.pop(tid)
            body=prefix+re.sub(r'^<\.\.\. \w+ resumed>', '', body)
            stamp=began
        call=re.match(r'(pwrite64|fdatasync|fsync|fallocate|ftruncate)\((.*)\)\s+=\s+(-?\d+).*?<([\d.]+)>$',body)
        if not call:
            unparsed.append(line);continue
        name,args,rc,duration=call[1],call[2],int(call[3]),float(call[4])
        pathpart=args.split('>',1)[0]
        pathpart=re.sub(r'\\x([0-9a-f]{2})',lambda m:chr(int(m[1],16)),pathpart)
        if not pathpart.endswith('/raft.log'):continue
        ev=dict(start=stamp,end=stamp+duration,name=name,args=args,rc=rc,duration=duration)
        if name=='pwrite64' and rc>0:
            data=re.search(r'"((?:\\x[0-9a-f]{2})+)"',args)
            tail=re.search(r', (\d+), (\d+)\s*$',args)
            if not data or not tail:
                unparsed.append(line);continue
            header=bytes.fromhex(data[1].replace('\\x',''))
            if len(header)<13 or int.from_bytes(header[4:8],'big')+8!=int(tail[1]) or rc!=int(tail[1]):
                unparsed.append(line);continue
            ev.update(type=header[8],group=int.from_bytes(header[9:13],'big'),bytes=rc,offset=int(tail[2]))
        events.append(ev)
    timeline=[]
    for ev in events:
        if ev['name']=='pwrite64' and ev['rc']>0:timeline.append((ev['end'],0,'write',ev))
        elif ev['name']=='fdatasync':timeline.append((ev['start'],1,'sync',ev))
    timeline.sort(key=lambda x:(x[0],x[1]))
    pending=[];cohorts=[];first=True
    for _,_,kind,ev in timeline:
        if kind=='write':pending.append(ev);continue
        batch=pending;pending=[]
        if first:first=False;continue
        if ev['rc']!=0:continue
        cohorts.append(dict(start=ev['start'],duration_ms=1000*ev['duration'],entries=sum(e['type']==2 for e in batch),records=len(batch),bytes=sum(e['bytes'] for e in batch),groups=sorted(set(e['group'] for e in batch)),offsets=[e['offset'] for e in batch]))
    entries=[c['entries'] for c in cohorts]
    return dict(trace=path.name,cohorts=cohorts,summary=dict(barriers=len(cohorts),entry_writes=sum(entries),entries_per_barrier=metrics(entries),records_per_barrier=metrics([c['records'] for c in cohorts]),bytes_per_barrier=metrics([c['bytes'] for c in cohorts]),sync_ms=metrics([c['duration_ms'] for c in cohorts]),zero_entry_barriers=sum(n==0 for n in entries),single_entry_barriers=sum(n==1 for n in entries),multi_entry_barriers=sum(n>1 for n in entries),multi_group_barriers=sum(len(c['groups'])>1 for c in cohorts),record_types=dict(collections.Counter(e.get('type') for e in events if e['name']=='pwrite64' and e['rc']>0)),unparsed=len(unparsed),unfinished=len(partial),errors=sum(e['rc']<0 for e in events)),unparsed_examples=unparsed[:5])

results=[trace(p) for p in sorted(base.glob('w*-n*.trace'))]
(base/'analysis.json').write_text(json.dumps(results,indent=2))
for r in results:print(r['trace'],json.dumps(r['summary']))

samples=[json.loads(line) for line in (base/'samples.jsonl').read_text().splitlines()]
def disks(s):
    return {p[2]:list(map(int,p[3:])) for line in s['diskstats'].splitlines() if len(p:=line.split())>=14}
def window(lo,hi):
    a=min(samples,key=lambda s:abs(s['seconds']-lo));b=min(samples,key=lambda s:abs(s['seconds']-hi))
    dt=b['seconds']-a['seconds'];out=dict(start=a['seconds'],end=b['seconds'])
    if dt<=0:return out
    if 'progress' in a and 'progress' in b:out['files_per_second']=(b['progress']['created']-a['progress']['created'])/dt
    da,db=disks(a),disks(b)
    for dev in ['sda','nvme0n1']:
        d=[y-x for x,y in zip(da[dev],db[dev])]
        out[dev]=dict(busy_percent=d[9]/dt/10,write_iops=d[4]/dt,write_mib_s=d[6]*512/dt/1048576,write_await_ms=d[7]/d[4] if d[4] else None,flush_await_ms=d[16]/d[15] if len(d)>16 and d[15] else None)
    return out
windows={label:window(lo,hi) for label,lo,hi in [('first100',0,100),('middle100',100,200),('last100',200,300),('pretrace',5,30),('trace0',30,60),('untraced0',60,130),('trace1',130,160),('untraced1',160,230),('trace2',230,260),('posttrace',260,300)] if samples[-1]['seconds']>=hi-.5}
(base/'windows.json').write_text(json.dumps(windows,indent=2))
print(json.dumps(windows,indent=2))
