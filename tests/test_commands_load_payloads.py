#!/usr/bin/env python3
"""Exercise the actual bounded payload oracle without launching a cluster."""
import ast
import concurrent.futures
import hashlib
from pathlib import Path
import tempfile
import threading
import time
from unittest.mock import patch

source=Path(__file__).resolve().parent/'live/efs_commands_load.py'
tree=ast.parse(source.read_text())
functions=[n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name in ('verify_payload_folder','verify_payloads')]
assert len(functions)==2
scope={'hashlib':hashlib,'concurrent':concurrent}
exec(compile(ast.Module(body=functions,type_ignores=[]),str(source),'exec'),scope)
verify=scope['verify_payloads']
with tempfile.TemporaryDirectory() as tmp:
 root=Path(tmp);fixture='payloads';counts=[1,2,3,4,1,2,3,4]
 for worker,count in enumerate(counts):
  folder=root/fixture/str(worker);folder.mkdir(parents=True)
  for index in range(count):(folder/str(index)).write_bytes(bytes([worker])*256)
 original=Path.read_bytes;lock=threading.Lock();ready=threading.Event();active=0;peak=0;seen=[]
 def observed_read(path):
  global active,peak
  with lock:
   active+=1;peak=max(peak,active);seen.append(path)
   if active==4:ready.set()
  try:
   assert ready.wait(3),'four readers did not overlap'
   time.sleep(.005)
   return original(path)
  finally:
   with lock:active-=1
 with patch.object(Path,'read_bytes',observed_read):assert verify(root,fixture,counts)==sum(counts)
 assert peak==4 and len(seen)==len(set(seen))==sum(counts),(peak,seen)
 damaged=root/fixture/'7'/'3';damaged.write_bytes(bytes([6])*256)
 try:verify(root,fixture,counts)
 except AssertionError as exc:assert exc.args[0]==(7,3),exc
 else:raise AssertionError('same-size corruption accepted')
 damaged.write_bytes(b'x')
 try:verify(root,fixture,counts)
 except AssertionError:pass
 else:raise AssertionError('short payload accepted')
 damaged.unlink()
 try:verify(root,fixture,counts)
 except FileNotFoundError:pass
 else:raise AssertionError('missing payload accepted')
print('bounded loaded-query payload oracle: exact coverage, four readers, corruption/short/missing failures PASS')
