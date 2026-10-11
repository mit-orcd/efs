#!/usr/bin/env python3
"""W97 bounded file-level durability control; no EFS or raw-device writes.

Three writers each synchronously persist 64 KiB. Compare repeated allocation
after 256 MiB rotations against overwriting a preallocated 256 MiB ring.
This is an allocation/rotation principle test, not an EFS workload substitute.
"""
import argparse,concurrent.futures,hashlib,json,os,shutil,threading,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path);p.add_argument('output',type=Path)
p.add_argument('--seconds',type=int,default=480)
p.add_argument('--mode',choices=('grow','ring'),required=True)
a=p.parse_args();a.root.mkdir(exist_ok=False);a.output.mkdir(exist_ok=False)
size=65536;slots=4096;gate=threading.Barrier(4);rows=[];stop=threading.Event()
seed=hashlib.sha256(b'NUC durable allocation control').digest()*2048
def worker(n):
    path=a.root/f'writer{n}';fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
    try:
        if a.mode=='ring':os.posix_fallocate(fd,0,size*slots)
        os.fsync(fd)
        d=os.open(a.root,os.O_DIRECTORY);os.fsync(d);os.close(d)
    except BaseException:
        gate.abort();os.close(fd);raise
    count=0;rotations=0;sync_ns=0;rotation_ns=0;hist=[0]*100001;maximum=0
    try:
        gate.wait(timeout=120);origin=time.monotonic();last=origin;last_count=0;last_ns=0
        while time.monotonic()-origin<a.seconds and not stop.is_set():
            if a.mode=='grow' and count and count%slots==0:
                t=time.monotonic_ns();os.ftruncate(fd,0);os.fdatasync(fd)
                rotations+=1;rotation_ns+=time.monotonic_ns()-t
            body=count.to_bytes(8,'little')+seed[8:]
            offset=(count%slots)*size
            assert os.pwrite(fd,body,offset)==len(body),'short durable-control write'
            t=time.monotonic_ns();os.fdatasync(fd);ns=time.monotonic_ns()-t
            sync_ns+=ns;maximum=max(maximum,ns);hist[min(ns//1000,100000)]+=1;count+=1
            now=time.monotonic()
            if now-last>=30:
                assert shutil.disk_usage(a.root).free>80*2**30
                row=dict(writer=n,seconds=now-origin,operations_s=(count-last_count)/(now-last),sync_mean_ms=(sync_ns-last_ns)/1e6/(count-last_count),count=count,rotations=rotations)
                with (a.output/f'writer{n}.jsonl').open('a') as out:out.write(json.dumps(row)+'\n')
                last=now;last_count=count;last_ns=sync_ns
        elapsed=time.monotonic()-origin
        os.close(fd);fd=-1
        # New descriptor, complete current file. This is close/reopen, not a
        # kernel-cold/power-loss oracle. Preserve every file after verification.
        verify_slots=min(count,slots) if a.mode=='ring' else (count%slots or min(count,slots))
        with path.open('rb',buffering=0) as source:
            for slot in range(verify_slots):
                version=((count-1-slot)//slots)*slots+slot
                assert source.read(size)==version.to_bytes(8,'little')+seed[8:]
        def pct(p):
            total=0
            for i,v in enumerate(hist):
                total+=v
                if total>=(count*p+99)//100:return i
        st=path.stat()
        row=dict(writer=n,count=count,seconds=elapsed,operations_s=count/elapsed,sync_mean_ms=sync_ns/1e6/count,sync_p50_us=pct(50),sync_p95_us=pct(95),sync_p99_us=pct(99),histogram_overflow_ge100ms=hist[-1],sync_max_ms=maximum/1e6,rotations=rotations,rotation_seconds=rotation_ns/1e9,reopened_exact_blocks=verify_slots,logical_bytes=st.st_size,allocated_bytes=st.st_blocks*512)
        (a.output/f'writer{n}-final.json').write_text(json.dumps(row,indent=2));rows.append(row)
    except BaseException:
        stop.set();raise
    finally:
        if fd>=0:os.close(fd)
cpu_before=os.times()
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    futures=[pool.submit(worker,n) for n in range(3)]
    gate.wait(timeout=120)
    with (a.output/'host.jsonl').open('w') as out:
        while any(not f.done() for f in futures):
            out.write(json.dumps(dict(monotonic=time.monotonic(),utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),diskstats=Path('/proc/diskstats').read_text(),host_stat=Path('/proc/stat').read_text(),process_stat=Path('/proc/self/stat').read_text(),process_io=Path('/proc/self/io').read_text(),meminfo=Path('/proc/meminfo').read_text()))+'\n');out.flush()
            time.sleep(5)
    for f in futures:f.result()
cpu_after=os.times()
(a.output/'final.json').write_text(json.dumps(dict(mode=a.mode,seconds=a.seconds,writers=rows,total_operations_s=sum(r['operations_s'] for r in rows),verified=True,cpu_user_seconds=cpu_after.user-cpu_before.user,cpu_system_seconds=cpu_after.system-cpu_before.system),indent=2))
print(a.mode,sum(r['operations_s'] for r in rows),'PASS',flush=True)
