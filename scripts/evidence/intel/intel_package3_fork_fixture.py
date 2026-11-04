#!/usr/bin/env python3
"""Small independent W106 payload fixture for efs_direct_read_fork.c.

Run prepare on one Intel mount, then diagnostic ROOT 0 128 128 on either host.
Check every path/byte/EOF warm and cold. Delete only this harness-owned fixture.
"""
import argparse,hashlib,os
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('phase',choices=['prepare','verify','delete']);p.add_argument('root',type=Path);p.add_argument('--count',type=int,default=128);a=p.parse_args()
assert 1<=a.count<=10000
assert a.root.name.startswith('package3-fork-')
if a.phase=='prepare':a.root.mkdir()
for w in range(8):
 directory=a.root/('worker-%02u'%w)
 if a.phase=='prepare':directory.mkdir()
 for off in range(a.count):
  index=a.count*w+off;d=directory/('dir-%06u'%(off//1024));path=d/('file-%09u'%index)
  if a.phase=='prepare':
   d.mkdir(exist_ok=True);seed=hashlib.sha256(b'W92-real-payload:'+index.to_bytes(8,'big')).digest();data=seed*8192
   with path.open('xb') as f:f.write(data);f.flush();os.fsync(f.fileno())
  elif a.phase=='verify':
   seed=hashlib.sha256(b'W92-real-payload:'+index.to_bytes(8,'big')).digest()
   assert path.stat().st_size==262144,path
   with path.open('rb',buffering=0) as f:
    for part in range(4):assert f.read(65536)==seed*2048,(path,part)
    assert not f.read(1),path
  else:path.unlink()
 if a.phase=='delete':
  for d in directory.iterdir():d.rmdir()
  directory.rmdir()
if a.phase=='delete':a.root.rmdir()
print(a.phase,'PASS',a.count*8,'files')
