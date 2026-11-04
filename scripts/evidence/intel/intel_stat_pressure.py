#!/usr/bin/env python3
"""Bounded read-only metadata pressure: stat known existing files, never read bodies."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import threading
import time
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('mount',type=Path)
ap.add_argument('--seconds',type=int,default=180)
ap.add_argument('--stop-file',type=Path,required=True)
a=ap.parse_args()
assert 1 <= a.seconds <= 300 and not a.stop_file.exists()
base=a.mount/'tiny-intel2-1m-20261010'
paths=[base/p for p in ['folder_115666/file_000000.txt','folder_115402/file_000000.txt',
 'folder_115402/file_000001.txt','file_000002.txt','file_000001.txt','file_000000.txt']]
expected=[(s.st_ino,s.st_size) for s in map(os.stat,paths)]
barrier=threading.Barrier(4)
end=time.monotonic()+a.seconds
print(json.dumps(dict(kind='ready',time=time.time(),paths=list(map(str,paths)),expected=expected)),flush=True)
def run(worker):
    count=0;errors=[];barrier.wait();start=time.monotonic()
    while time.monotonic()<end and not a.stop_file.exists():
        i=(count+worker)%len(paths)
        try:
            s=os.stat(paths[i]);assert (s.st_ino,s.st_size)==expected[i]
        except Exception as e:
            errors.append(str(e));break
        count+=1
    return dict(worker=worker,count=count,seconds=time.monotonic()-start,errors=errors)
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    rows=list(pool.map(run,range(4)))
print(json.dumps(dict(kind='complete',time=time.time(),workers=rows)),flush=True)
if any(r['errors'] for r in rows):raise SystemExit(1)
