#!/usr/bin/env python3
"""Bounded private-fixture writer/publication gate; never use an existing tree."""
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import sys
import time

mount=Path(sys.argv[1])
peer=Path(sys.argv[2])
root=mount/'command-load'
root.mkdir()  # refuse reuse
end=time.monotonic()+14

def writer(worker):
    count=0
    folder=root/str(worker)
    folder.mkdir()
    while time.monotonic()<end and count<2000:
        # Actual small payload and close, rather than namespace-only RPCs.
        (folder/str(count)).write_bytes(bytes([worker])*256)
        count+=1
    return count

generations=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
    tasks=[pool.submit(writer,i) for i in range(8)]
    for _ in range(5):
        time.sleep(2)
        result=subprocess.run([sys.executable,str(peer/'.efs/stats'),'--json'],
                              check=True,stdout=subprocess.PIPE,timeout=12)
        snap=json.loads(result.stdout)['snapshot']
        generations.append(snap['generation'])
        print('loaded snapshot '+json.dumps(snap,sort_keys=True),flush=True)
    counts=[t.result(timeout=30) for t in tasks]
assert sum(counts)>100,counts
assert len(set(generations))>1,('publication starved under ordinary writes',generations)
# After close, wait for the exact independent writer oracle.
expected=sum(counts)
for _ in range(40):
    result=subprocess.run([sys.executable,str(peer/'command-load/.efs/stats'),'--json'],
                          check=True,stdout=subprocess.PIPE,timeout=12)
    parsed=json.loads(result.stdout)
    if parsed['stats']['tree_files']==expected:
        assert parsed['stats']['tree_bytes']==expected*256,parsed
        break
    time.sleep(.25)
else:
    raise AssertionError(('closed writer oracle did not publish',expected,parsed))
print(f'loaded publication PASS files={expected} logical_bytes={expected*256}',flush=True)
