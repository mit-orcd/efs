#!/usr/bin/env python3
"""Bounded nonzero dd+fsync comparison; owns only a new benchmark directory.

Preserve the local fixture between baseline/candidate. Never pass a human file
as the output. No feature, process or service changes. Re-read verifies data
outside the timed write; output cleanup is limited to files created here.
"""
import argparse, hashlib, json, os, subprocess, time
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('mount',type=Path);p.add_argument('artifacts',type=Path)
p.add_argument('phase',choices=['baseline','candidate']);p.add_argument('--mib',type=int,default=512)
p.add_argument('--repeats',type=int,default=3)
a=p.parse_args();assert 32<=a.mib<=4096 and 1<=a.repeats<=5
assert subprocess.check_output(['findmnt','-rn','-M',str(a.mount),'-o','FSTYPE'],text=True).strip()=='fuse.efs-fuse'
assert not (a.mount/'.efs').exists()
a.artifacts.mkdir(parents=True,exist_ok=True)
fixture=a.artifacts/f'random-{a.mib}MiB.bin'
if not fixture.exists():
 with fixture.open('xb') as f:
  for _ in range(a.mib):f.write(os.urandom(1024*1024))
  f.flush();os.fsync(f.fileno())
assert fixture.stat().st_size==a.mib*1024*1024
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  while True:
   block=f.read(1024*1024)
   if not block:break
   h.update(block)
 return h.hexdigest()
sha=digest(fixture)
run=a.mount/f'dd-sync-batch-{a.phase}-{time.time_ns()}'
run.mkdir()
for i in range(a.repeats):
 target=run/f'write-{i}.bin'
 cmd=['dd',f'if={fixture}',f'of={target}','bs=1M',f'count={a.mib}','conv=fsync,excl','status=none']
 start=time.monotonic();r=subprocess.run(cmd,capture_output=True,text=True,timeout=300);elapsed=time.monotonic()-start
 assert r.returncode==0,r.stderr
 assert target.stat().st_size==fixture.stat().st_size
 assert digest(target)==sha
 print(json.dumps(dict(phase=a.phase,trial=i,mib=a.mib,seconds=elapsed,mib_s=a.mib/elapsed,sha256=sha,verified=True,command=cmd)),flush=True)
 target.unlink()
run.rmdir()
