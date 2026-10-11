#!/usr/bin/env python3
"""Large real FUSE mutations; query aggregates versus independent syscall/byte oracle.

Creates only a fresh namespace under caller-owned fixture mounts. Leaves it for
cold fixture verification; never changes server policy or production deadlines.
The deterministic 1,024 boundary is separately covered by test_query_delta.c.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mount',type=Path);p.add_argument('peer',type=Path)
p.add_argument('--metadata-only',action='store_true')
p.add_argument('--files',type=int,default=1025)
a=p.parse_args();assert 16<=a.files<=8192
mount,peer=a.mount,a.peer
assert os.path.ismount(mount) and os.path.ismount(peer)
name='query-large-delta-'+uuid.uuid4().hex
root=mount/name;other=peer/name
root.mkdir();count=a.files

def data(i,phase):
    if a.metadata_only:return b''
    return hashlib.sha256(f'{i}:{phase}'.encode()).digest()*(phase+1)

def write(i,phase):
    if a.metadata_only:
        if not phase:(root/f'file-{i:04d}').touch(exist_ok=False)
        stamp=(1577836800+phase*31622400)*10**9
        os.utime(root/f'file-{i:04d}',ns=(stamp,stamp))
        return
    with (root/f'file-{i:04d}').open('xb' if not phase else 'wb') as stream:
        stream.write(data(i,phase));stream.flush();os.fsync(stream.fileno())

def check(expected,bytes_expected):
    deadline=time.monotonic()+120
    while True:
        p=subprocess.run([sys.executable,str(other/'.efs/stats'),'--json'],capture_output=True,timeout=30)
        if not p.returncode:
            obj=json.loads(p.stdout)
            if (obj['stats']['tree_files']==expected and obj['stats']['tree_bytes']==bytes_expected
                    and obj['snapshot']['refresh_error']==0 and obj['complete']):
                print(json.dumps({'expected_files':expected,'expected_bytes':bytes_expected,'snapshot':obj['snapshot'],'build':obj.get('build')}),flush=True)
                return
        assert time.monotonic()<deadline,(p.returncode,p.stderr)
        time.sleep(.25)
for phase in range(2):
    start=time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(lambda i:write(i,phase),range(count)))
    check(count,count*len(data(0,phase)))
    if a.metadata_only:
        deadline=time.monotonic()+120
        while True:
            result=subprocess.run([sys.executable,str(other/'.efs/find'),'',
                                   '--type','file','--limit',str(count),'--json'],capture_output=True,timeout=30)
            obj=json.loads(result.stdout) if not result.returncode else {}
            entries=obj.get('entries',[])
            stamp=(1577836800+phase*31622400)*10**9
            if len(entries)==count and all(e['mtime_ns']==stamp and e['size']==0 for e in entries):
                assert obj['complete'],obj
                print('all indexed mtime/size fields agree with metadata mutation PASS',flush=True)
                break
            assert time.monotonic()<deadline,(result.returncode,result.stderr,len(entries))
            time.sleep(.25)
    def verify(i):
        if a.metadata_only:
            st=(other/f'file-{i:04d}').stat();assert st.st_size==0 and st.st_mtime_ns==(1577836800+phase*31622400)*10**9
        else:assert (other/f'file-{i:04d}').read_bytes()==data(i,phase)
    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        list(pool.map(verify,range(count)))
    print(f'phase={phase} acknowledged and peer-verified={count} elapsed_s={time.monotonic()-start:.3f}',flush=True)
os.rename(root/f'file-{count-1:04d}',root/'renamed')
os.link(root/f'file-{count-2:04d}',root/'alias')
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
    list(pool.map(lambda i:(root/f'file-{i:04d}').unlink(),range(min(512,count//2))))
# Compare the query with a separate live walk after the workload has stopped.
files=list(other.iterdir())
assert len(files)==count-min(512,count//2)+1
logical=sum(p.stat().st_size for p in files)
check(len(files),logical)
assert (other/'renamed').read_bytes()==data(count-1,1)
assert (other/'alias').read_bytes()==data(count-2,1)
print(('large delta' if count>1024 else 'mutation smoke')+' create/update/delete/rename/hard-link query and peer oracle PASS files='+str(count)+' mode='+('metadata' if a.metadata_only else 'payload'),flush=True)
