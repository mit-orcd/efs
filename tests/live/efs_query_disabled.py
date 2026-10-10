#!/usr/bin/env python3
"""Private fixture written while queries are disabled; independent cold oracle."""
import hashlib
import json
import os
from pathlib import Path
import sys
import time
from query_rpc import features, query, request
mount=Path(sys.argv[1])
phase=sys.argv[2]
assert phase in ('write','check','quota-write','quota-check')
quota=phase.startswith('quota-')
root=mount/('w92-quota-catchup-20261010' if quota else 'w92-disabled-catchup-20261010')
count=500
payload=lambda index:hashlib.sha256(('w92-disabled-%d'%index).encode()).digest()*128
if phase.endswith('write'):
    port=int(os.environ['EFS_QUERY_TEST_PORT'])
    mask=features(port)
    assert (mask!=0 if quota else mask==0),('capability policy',phase,mask)
    deadline=time.monotonic()+30
    while True:
        reply,text=query(port,request(version=2,operation=2,uid=os.getuid(),gid=os.getgid()))
        assert reply.status and not text,'unavailable publisher returned successful bytes'
        if not quota or reply.status==-11 or reply.refresh_error==-11:break
        assert time.monotonic()<deadline,('quota refusal did not become truthful',reply.status,reply.refresh_error)
        time.sleep(.25)
    print(json.dumps(dict(query_status=reply.status,refresh_error=reply.refresh_error,capabilities=mask)),flush=True)
    root.mkdir()
    for index in range(count):
        with open(root/('%04d.bin'%index),'xb') as stream:
            stream.write(payload(index));stream.flush();os.fsync(stream.fileno())
assert len(list(root.iterdir()))==count
for index in range(count):
    assert (root/('%04d.bin'%index)).read_bytes()==payload(index),index
print(json.dumps(dict(phase=phase,files=count,bytes=count*4096,payloads_verified=count)))
