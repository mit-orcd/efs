#!/usr/bin/env python3
"""Resumable W90 namespace + physical payload, bounded by host free space.
Only writes its own dedicated tree. Stop by touching CONTROL/PAUSE; restart
with identical arguments. A JSONL ledger records completed, fsynced files.
"""
import argparse,concurrent.futures,hashlib,json,os,signal,threading,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('mount');p.add_argument('--target-gib',type=int,default=380);p.add_argument('--small-files',type=int,default=131072);p.add_argument('--workers',type=int,default=4);p.add_argument('--control',default='/data1/efs/w90-control');a=p.parse_args()
root=Path(a.mount)/'w90-scale';control=Path(a.control);control.mkdir(parents=True,exist_ok=True);root.mkdir(exist_ok=True)
lock=threading.Lock();dir_lock=threading.Lock();known_dirs=set();stop=threading.Event();started=time.monotonic();completed=0;totalbytes=0
signal.signal(signal.SIGTERM,lambda *_:stop.set());signal.signal(signal.SIGINT,lambda *_:stop.set())
ledger=open(control/'completed.jsonl','a',buffering=1)
def check():
    if stop.is_set() or (control/'PAUSE').exists():raise InterruptedError('W90 population paused')
    for path in ['/data1/efs/n1','/data2/efs/n3']:
        st=os.statvfs(path)
        if st.f_bavail*st.f_frsize<50*(1<<30) or st.f_favail<100000:
            stop.set();raise RuntimeError('space/inode reserve reached: '+path)
def write(item):
    global completed,totalbytes
    rel,size=item;check();path=root/rel
    # The loader owns this dedicated tree. Avoid repeating five ancestor
    # mkdir RPCs for every file; the bounded namespace has ~2K leaf dirs.
    with dir_lock:
        if path.parent not in known_dirs:
            path.parent.mkdir(parents=True,exist_ok=True);known_dirs.add(path.parent)
    if path.exists():
        if path.stat().st_size!=size:raise RuntimeError('existing file size mismatch: '+str(path))
        return
    payload=hashlib.shake_256(rel.encode()).digest(min(size,1<<20));partial=path.with_name(path.name+'.partial')
    with open(partial,'wb',buffering=0) as f:
        left=size
        while left:
            check();chunk=payload[:min(left,len(payload))];n=f.write(chunk)
            if n!=len(chunk):raise IOError('short write')
            left-=n
        os.fsync(f.fileno())
    os.rename(partial,path)
    with lock:
        completed+=1;totalbytes+=size
        ledger.write(json.dumps(dict(path=rel,bytes=size,block_sha256=hashlib.sha256(payload).hexdigest()))+'\n')
        if completed%128==0:
            os.fsync(ledger.fileno())
            print(json.dumps(dict(files=completed,bytes=totalbytes,elapsed=round(time.monotonic()-started,1),phase='small' if rel.startswith('small/') else 'bulk')),flush=True)
def items():
    for i in range(a.small_files):
        yield f'small/partition-{i%128:03}/branch-{(i//128)%16:02}/level-a/level-b/report-{i:07}.dat',16384
    remaining=a.target_gib*(1<<30)-a.small_files*16384
    if remaining<0:raise ValueError('target below namespace payload')
    i=0
    while remaining:
        size=min(64<<20,remaining);yield f'bulk/group-{i%32:02}/payload-{i:05}.bin',size
        remaining-=size;i+=1
try:
    # Bound queued work; never materialize millions of Future objects.
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
        source=iter(items());pending=set()
        while True:
            while len(pending)<a.workers*2:
                try:item=next(source)
                except StopIteration:break
                pending.add(pool.submit(write,item))
            if not pending:break
            done,pending=concurrent.futures.wait(pending,return_when=concurrent.futures.FIRST_COMPLETED)
            for f in done:f.result()
    print(json.dumps(dict(status='COMPLETE',target_bytes=a.target_gib*(1<<30),new_files=completed,new_bytes=totalbytes,elapsed=time.monotonic()-started)),flush=True)
except BaseException as e:
    stop.set();print(json.dumps(dict(status='STOPPED',error=repr(e),new_files=completed,new_bytes=totalbytes)),flush=True);raise
finally:ledger.close()
