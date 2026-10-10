#!/usr/bin/env python3
"""Private publisher-loss fixture, durable acknowledgement and independent oracle."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from query_rpc import query, page_request
mount=Path(sys.argv[1]);phase=sys.argv[2];state=Path(sys.argv[3])
root=mount/'w92-interrupted-publisher-20261010'
port=int(os.environ['EFS_QUERY_TEST_PORT'])
payload=lambda i:hashlib.sha256(('w92-publisher-%d'%i).encode()).digest()*128

def put(index):
    with open(root/('%04d.bin'%index),'xb') as stream:
        stream.write(payload(index));stream.flush();os.fsync(stream.fileno())

def stats():
    p=subprocess.run([sys.executable,str(root/'.efs/stats'),'--json'],capture_output=True,timeout=20)
    assert p.returncode==0,(p.returncode,p.stderr)
    return json.loads(p.stdout)

if phase=='pin':
    root.mkdir()
    for i in range(100):put(i)
    deadline=time.monotonic()+40
    while True:
        result=stats()
        if result['stats']['tree_files']==100:break
        assert time.monotonic()<deadline,result
        time.sleep(.25)
    reply,text=query(port,page_request(root=root.stat().st_ino,uid=os.getuid(),gid=os.getgid(),limit=1))
    assert reply.status==0,(reply.status,reply.refresh_error)
    page=json.loads(text)['page']
    state.write_text(json.dumps(dict(root=root.stat().st_ino,token=page['token'],cursor=page['cursor'],boot=reply.boot_id,generation=reply.generation)))
    print('retained export pinned '+state.read_text(),flush=True)
elif phase=='write':
    ack=100;error=None
    try:
        for i in range(100,2100):put(i);ack=i+1
    except Exception as e:error=repr(e)
    finally:
        record=json.dumps(dict(acknowledged=ack,error=error))
        with open(str(state)+'.ack','w') as stream:
            stream.write(record);stream.flush();os.fsync(stream.fileno())
        print('writer verdict '+record,flush=True)
elif phase=='check':
    old=json.loads(state.read_text());ack=json.loads(Path(str(state)+'.ack').read_text())
    reply,text=query(port,page_request(root=old['root'],uid=os.getuid(),gid=os.getgid(),token=int(old['token']),cursor=int(old['cursor']),limit=1))
    assert reply.status and not text,'old publisher token returned successful bytes'
    for i in range(ack['acknowledged']):assert (root/('%04d.bin'%i)).read_bytes()==payload(i),i
    entries=list(root.iterdir());actual_bytes=sum(p.stat().st_size for p in entries)
    deadline=time.monotonic()+60
    while True:
        result=stats();totals=result['stats']
        if totals['tree_files']==len(entries) and totals['tree_bytes']==actual_bytes:break
        assert time.monotonic()<deadline,(totals,len(entries),actual_bytes)
        time.sleep(.25)
    assert result['snapshot']['boot_id']!=old['boot']
    print(json.dumps(dict(old_token_status=reply.status,acknowledged_verified=ack['acknowledged'],actual_files=len(entries),actual_bytes=actual_bytes,writer_error=ack['error'],snapshot=result['snapshot'])),flush=True)
else:raise ValueError(phase)
