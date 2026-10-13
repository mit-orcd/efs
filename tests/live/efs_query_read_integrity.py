#!/usr/bin/env python3
"""Read-only concurrent payload reproducer. Preserve mismatches outside EFS."""
import argparse,concurrent.futures,hashlib,json,os,threading,time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path);p.add_argument('output',type=Path)
p.add_argument('--total-files',type=int,default=1638400)
p.add_argument('--workers',type=int,default=8)
p.add_argument('--per-worker',type=int,default=1024)
p.add_argument('--rounds',type=int,default=3)
p.add_argument('--bytes',type=int,default=262144)
a=p.parse_args();a.root=a.root.resolve();a.output=a.output.resolve()
assert not a.output.is_relative_to(a.root.parent)
a.output.mkdir(parents=True,exist_ok=False)
stop=threading.Event();lock=threading.Lock();count=[0];failures=[];started=time.monotonic()
def worker(number):
 begin=a.total_files*number//a.workers
 for round_ in range(a.rounds):
  for offset in range(a.per_worker):
   if stop.is_set():return
   index=begin+offset;target=a.root/('worker-%02d'%number)/('dir-%06d'%(offset//1024))/('file-%09d'%index)
   expected=hashlib.sha256(b'W92-real-payload:'+index.to_bytes(8,'big')).digest()*(a.bytes//32)
   chunks=[]
   with target.open('rb',buffering=0) as f:
    while True:
     chunk=f.read(65536)
     if not chunk:break
     chunks.append(chunk)
   data=b''.join(chunks)
   if data!=expected:
    with lock:
     name='mismatch-%02d-%d-%d.bin'%(number,round_,index)
     (a.output/name).write_bytes(data)
     record=dict(worker=number,round=round_,index=index,path=str(target),size=target.stat().st_size,length=len(data),chunk_lengths=[len(x) for x in chunks],hash=hashlib.sha256(data).hexdigest(),expected_hash=hashlib.sha256(expected).hexdigest(),first=data[:64].hex(),expected_first=expected[:64].hex(),chunk_hashes=[hashlib.sha256(x).hexdigest() for x in chunks],dump=name)
     failures.append(record);stop.set();(a.output/'failures.json').write_text(json.dumps(failures,indent=2)+'\n')
    return
   with lock:count[0]+=1
with concurrent.futures.ThreadPoolExecutor(max_workers=a.workers) as pool:
 for f in [pool.submit(worker,n) for n in range(a.workers)]:f.result()
result=dict(result='FAIL' if failures else 'PASS',reads=count[0],seconds=time.monotonic()-started,workers=a.workers,per_worker=a.per_worker,rounds=a.rounds,failures=failures)
(a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
raise SystemExit(bool(failures))
