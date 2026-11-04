#!/usr/bin/env python3
"""Record per-node snapshot visibility after helper warmup; retain the fixture.
Requires tests/live on PYTHONPATH and the campaign query endpoint environment.
This is diagnostic evidence, not a passing acceptance gate.
"""
import json,os,pathlib,time,uuid
from query_rpc import query,request
from query_timing import wait_indexed,refresh_seconds,settle_seconds
mount=pathlib.Path('/tmp/efs-mount')
root=mount/('query-node-probe-'+uuid.uuid4().hex)
root.mkdir()
ino=root.stat().st_ino
warm=wait_indexed(root,settle_seconds(refresh_seconds(mount)))
print(json.dumps(dict(event='warm',fixture=str(root),root=ino,snapshot=warm['snapshot'])),flush=True)
nodes=json.loads(os.environ['EFS_QUERY_TEST_NODES'])
for step in range(45):
 for node,(host,port) in sorted(nodes.items()):
  try:
   reply,text=query(port,request(version=2,operation=1,root=ino,uid=os.getuid(),gid=os.getgid(),match=3,max_output=1<<20),host=host)
   print(json.dumps(dict(step=step,node=node,status=reply.status,primary=reply.primary_id,boot=reply.boot_id,generation=reply.generation,terms=list(reply.terms),checked_dirs=reply.checked_dirs,refresh_error=reply.refresh_error)),flush=True)
  except Exception as e:print(json.dumps(dict(step=step,node=node,error=repr(e))),flush=True)
 time.sleep(2)
