#!/usr/bin/env python3
"""Same 14s writer fixture, one raw reader per supplied dual-query target.

Uses two existing native mounts; never changes policy or starts/stops daemons.
Retains files on failure. Redirects are deliberately bypassed for diagnosis.
Record the installed source, actual roles and feature profile separately.
"""
import argparse
import os
from pathlib import Path
import sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mount');p.add_argument('peer')
p.add_argument('--targets',nargs=2,required=True)
p.add_argument('--port',type=int,default=17432)
p.add_argument('--fixture',required=True)
a=p.parse_args()
assert os.path.ismount(a.mount) and os.path.ismount(a.peer)
assert a.fixture and '/' not in a.fixture and a.fixture not in ('.','..')
os.environ['EFS_QUERY_TEST_FIXTURE']=a.fixture
os.environ['EFS_QUERY_TEST_PORT']=str(a.port)
source=Path(__file__).with_name('efs_commands_load.py')
replacement='''import threading
from query_rpc import query as raw_query, request
local=threading.local();targets=iter(TARGETS);target_lock=threading.Lock()
def query(port,q):
 if not hasattr(local,'target'):
  with target_lock:local.target=next(targets)
  local.sampled=0;local.successes=0
 r,b=raw_query(port,q,host=local.target)
 if local.sampled<15 or (r.status==0 and local.successes<3):
  print('TARGET_HEADER',json.dumps(dict(target=local.target,rc=r.status,boot=r.boot_id,generation=r.generation,checked_dirs=r.checked_dirs,primary=r.primary_id,bytes=len(b))),flush=True)
  local.sampled+=1
 if not r.status:local.successes+=1
 return r,b
'''.replace('TARGETS',repr(a.targets))
text=source.read_text();needle='from query_rpc import routed_query as query, request'
assert needle in text
sys.argv=[str(source),a.mount,a.peer]
exec(compile(text.replace(needle,replacement),str(source),'exec'),
     dict(__name__='__main__',__file__=str(source)))
