#!/usr/bin/env python3
"""Reduce path-resolved sync calls, retaining unfinished/resumed coverage."""
import argparse,collections,json,re
from pathlib import Path

def reduce_trace(trace,roots):
    pending={};groups=collections.defaultdict(list);coverage=collections.Counter()
    for line in trace.splitlines():
        match=re.match(r'^(\d+)\s+[\d.]+\s+(.*)$',line)
        if not match:continue
        tid,call=match.groups()
        start=re.match(r'(fsync|fdatasync)\((\d+)(?:<([^>]+)>)?',call)
        resumed=re.match(r'<\.\.\. (fsync|fdatasync) resumed>',call)
        if start:
            kind,fd,path=start.groups();path=path or '<unknown>'
            if '<unfinished ...>' in call:
                if tid in pending:coverage['overwritten_pending']+=1
                pending[tid]=(kind,path);coverage['unfinished_starts']+=1;continue
        elif resumed:
            coverage['resumed_lines']+=1
            previous=pending.pop(tid,None)
            if previous is None or previous[0]!=resumed[1]:
                coverage['unmatched_resumed']+=1;continue
            kind,path=previous;coverage['paired_resumed']+=1
        else:continue
        finish=re.search(r'=\s+(-?\d+).*<([\d.]+)>$',call)
        if not finish:coverage['missing_return_or_duration']+=1;continue
        result=int(finish[1]);seconds=float(finish[2]);coverage['completed_calls']+=1
        if result:coverage['failed_calls']+=1
        device='sata' if path.startswith('/data1/') else 'nvme' if path.startswith('/home/efs/') else 'shared/unknown'
        if '/mdraft/log/raft.log' in path:role='raft-log'
        elif '/mdraft/kv/wal' in path:role='kv-wal'
        elif re.search(r'/mdraft/applied\.\d+$',path):role='applied-marker'
        elif any(root.startswith(path.rstrip('/')+'/') for root in roots) or path=='/':role='ancestor-above-storage-root'
        elif path in roots:role='storage-root'
        elif any(path.startswith(root+'/data/') or path==root+'/data' for root in roots):
            role='fragment-file' if re.search(r'/\d+\.\d+\.\d+$',path) else 'fragment-directory'
        else:role='other/unknown'
        groups[(kind,device,role)].append(seconds)
    coverage['pending_at_detach']=len(pending)
    rows=[]
    for (kind,device,role),values in sorted(groups.items()):
        ordered=sorted(values)
        rows.append(dict(syscall=kind,device_path=device,path_class=role,calls=len(values),
            pooled_thread_seconds=sum(values),mean_ms=1000*sum(values)/len(values),
            p95_ms=1000*ordered[min(len(values)-1,int(.95*len(values)))]))
    return dict(coverage=dict(coverage),groups=rows,limits='Completed syscall durations include ptrace overhead and all thread waits, not wall percentages or device service time. Classes use trace path shapes and recorded storage roots, not stat/NAND measurements. Unfinished calls are counted only when paired with their matching resumed return.')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path);args=parser.parse_args()
    before=json.loads((args.directory/'before.json').read_text())
    roots=[re.search(r'--storage (\S+)',p['command'])[1] for p in before['processes'] if p['kind']=='efsd']
    result=reduce_trace((args.directory/'sync-paths.trace').read_text(),roots)
    (args.directory/'sync-paths-summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))
