#!/usr/bin/env python3
"""Offline perf-script PUT stage analysis; excludes incomplete boundary jobs.
Usage: summarize_put_stages.py events.txt. See capture_intel_put_stages.sh.
Durations are monotonic host-local; never subtract clocks across hosts.
"""
import collections
import json
from pathlib import Path
import re


def stats(values):
    v = sorted(values)
    if not v:
        return {'count': 0}
    return dict(count=len(v), mean_ms=sum(v)/len(v)*1000,
                p50_ms=v[(len(v)-1)//2]*1000,
                p95_ms=v[int((len(v)-1)*.95)]*1000,
                max_ms=v[-1]*1000)


def overlap(intervals):
    events = sorted((t, delta) for a,b in intervals if b >= a
                    for t,delta in [(a,1),(b,-1)])
    if not events:
        return {}
    active=peak=0; last=events[0][0]; bins=collections.defaultdict(float)
    for t,delta in events:
        bins[active] += t-last
        active += delta; peak=max(peak,active); last=t
    span=events[-1][0]-events[0][0]
    return dict(span_s=span, peak=peak,
                mean_active=sum(k*v for k,v in bins.items())/span if span else 0,
                seconds_by_active=dict(sorted(bins.items())))


def queue_service_overlap(jobs, workers=4):
    events=[]
    for j in jobs:
        if 'admit' in j:
            events.extend([(j['start'],1,0),(j['admit'],-1,0)])
        if 'run' in j and 'done' in j:
            events.extend([(j['run'],0,1),(j['done'],0,-1)])
    events.sort()
    if not events: return {}
    q=s=0;last=events[0][0];bins=collections.defaultdict(float)
    for t,dq,ds in events:
        bins[q,s]+=t-last;q+=dq;s+=ds;last=t
    return dict(queued_with_idle_worker_s=sum(v for (q,s),v in bins.items() if q>0 and s<workers),
                at_least_four_queued_with_idle_worker_s=sum(v for (q,s),v in bins.items() if q>=4 and s<workers),
                queued_with_no_active_worker_s=sum(v for (q,s),v in bins.items() if q>0 and s==0),
                queued_with_all_workers_busy_s=sum(v for (q,s),v in bins.items() if q>0 and s==workers))


def lines(path):
    with path.open(errors="replace") as stream:
        yield from stream


def summarize(path, ino=None, skip_seconds=0):
    # perf -F comm,pid,tid,time,event,trace prints PID/TID then timestamp.
    row=re.compile(r'^\s*.*?\s+(\d+)/(\d+)\s+(\d+\.\d+):\s+(\S+):\s*(.*)$')
    jobs={}; callers={}; workers={}; calls={}; complete=[]
    counts=collections.Counter(); spans=collections.defaultdict(list)
    first_time=None
    for line in lines(path):
        if 'LOST' in line.upper(): counts['lost_event_lines']+=1
        m=row.match(line)
        if not m:
            counts['unparsed_lines']+=1; continue
        pid,tid=int(m[1]),int(m[2]);t=float(m[3]);event=m[4];body=m[5]
        if first_time is None: first_time=t
        if t < first_time+skip_seconds: continue
        thread=(pid,tid)
        fields={k:int(v,16) if '0x' in v else int(v)
                for k,v in re.findall(r'\b(\w+)=(-?0x[0-9a-fA-F]+|-?\d+)',body)}
        if event.startswith('efs_putstage:'):
            name=event.split(':',1)[1].removesuffix('__return');counts[name]+=1
            if name=='submit':
                key=(pid,fields['job']);job=dict(pid=pid,ino=fields.get('ino'),
                    start=t,syscalls=collections.defaultdict(list))
                jobs[key]=job;callers[thread]=key
            elif name=='admitted':
                if (pid,fields['job']) in jobs:jobs[pid,fields['job']]['admit']=t
            elif name=='run':
                key=(pid,fields['job'])
                if key in jobs:
                    jobs[key]['run']=t;workers[thread]=key
            elif name in ('finish','dir','dir_done','done'):
                key=workers.get(thread)
                if key in jobs:
                    jobs[key][name]=t
                    if name=='done': workers.pop(thread,None)
            elif name=='submitted':
                key=callers.pop(thread,None);j=jobs.pop(key,None)
                if j:
                    j['end']=t;j['rc']=fields.get('rc');complete.append(j)
        elif event.startswith('syscalls:'):
            name=event.split(':',1)[1]
            if name.startswith('sys_enter_'):
                key=workers.get(thread)
                if key in jobs:calls[thread]=(key,name[10:],t)
            elif name.startswith('sys_exit_'):
                pending=calls.pop(thread,None)
                if pending:
                    key,op,start=pending
                    if key in jobs and op==name[9:]:
                        j=jobs[key]
                        phase='directory' if 'dir' in j else ('file' if 'finish' in j else 'write')
                        j['syscalls'][phase+'/'+op].append((start,t))
                        if ino is None or j['ino'] == ino:
                            spans[str(pid)+'/'+phase+'/'+op].append((start,t))
    groups=collections.defaultdict(list)
    for j in complete:
        if ino is None or j['ino'] == ino: groups[str(j['pid'])].append(j)
    result={}
    for pid,js in groups.items():
        metrics=collections.defaultdict(list);rcs=collections.Counter()
        for j in js:
            rcs[str(j['rc'])]+=1
            for name,a,b in [('total','start','end'),('admission','start','admit'),
                  ('dispatch','admit','run'),('service','run','done'),
                  ('before_file_barrier','run','finish'),('file_barrier_and_close','finish','dir'),
                  ('directory_path','dir','dir_done'),('caller_resume','done','end')]:
                if a in j and b in j:metrics[name].append(j[b]-j[a])
            for name,intervals in j['syscalls'].items():
                metrics[name].append(sum(b-a for a,b in intervals))
        result[pid]=dict(queue_service_overlap=queue_service_overlap(js), completed=len(js),return_codes=dict(rcs),
              stages={k:stats(v) for k,v in metrics.items()},
              service_overlap=overlap([(j['run'],j['done']) for j in js if 'run' in j and 'done' in j]),
              queue_overlap=overlap([(j['start'],j['admit']) for j in js if 'admit' in j]))
    return dict(processes=result,syscall_overlap={k:overlap(v) for k,v in spans.items()},
                diagnostics=dict(counts),unfinished_jobs=len(jobs),
                completed_by_inode=dict(collections.Counter(str(j['ino']) for j in complete)))


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace',type=Path)
    parser.add_argument('--ino',type=int,help='Only this workload inode; default all PUTs')
    parser.add_argument('--skip-seconds',type=float,default=0)
    args=parser.parse_args()
    print(json.dumps(summarize(args.trace,args.ino,args.skip_seconds),indent=2))
