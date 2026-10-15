#!/usr/bin/env python3
"""Two concurrent 64 MiB writers: background pressure, two fsyncs, exact EOF.

Creates only a unique directory under the supplied mounted export. Leaves its
files on failure; removes them only after both writers pass full verification.
"""
import concurrent.futures, hashlib, json, os, subprocess, sys, time
from pathlib import Path
mount=Path(sys.argv[1])
assert subprocess.check_output(['findmnt','-rn','-M',str(mount),'-o','FSTYPE'],text=True).strip()=='fuse.efs-fuse'
assert not os.path.lexists(mount/'.efs')
root=mount/f'pressure-smoke-{time.time_ns()}'
root.mkdir()
def trial(role):
 block=hashlib.shake_256(f'pressure-smoke-{role}'.encode()).digest(1024*1024)
 p=root/f'writer-{role}'
 with p.open('xb',buffering=0) as f:
  for _ in range(64):assert f.write(block)==len(block)
  os.fsync(f.fileno());os.fsync(f.fileno())
  assert os.fstat(f.fileno()).st_size==64*len(block)
 with p.open('rb',buffering=0) as f:
  for _ in range(64):assert f.read(len(block))==block
  assert not f.read(1)
 with p.open('r+b',buffering=0) as f:
  f.truncate(64*len(block)-7);os.fsync(f.fileno())
  f.truncate(64*len(block)+13);os.fsync(f.fileno());os.fsync(f.fileno())
 with p.open('rb',buffering=0) as f:
  for _ in range(63):assert f.read(len(block))==block
  assert f.read(len(block)+14)==block[:-7]+bytes(20)
  assert not f.read(1)
 assert p.stat().st_size==64*len(block)+13
 return dict(role=role,mib=64,two_fsync=True,shrink_regrow=True,contents=True,EOF=True)
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
 rows=list(pool.map(trial,range(2)))
for row in rows:print(json.dumps(row),flush=True)
for role in range(2):(root/f'writer-{role}').unlink()
root.rmdir()
