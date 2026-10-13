#!/usr/bin/env python3
"""Exercise natural Raft snapshot thresholds with ordinary POSIX xattr updates.

Private fresh directory, selected inode parity/group, verified 4 KiB attributes.
No changes to snapshot thresholds, timeouts, quotas or filesystem flags.
Run alongside foreground latency probes; retain raw output outside EFS.
"""
import argparse,json,os,time,hashlib
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('mount',type=Path);p.add_argument('output',type=Path);p.add_argument('--group',type=int,choices=[0,2],required=True);p.add_argument('--seconds',type=float,default=600);a=p.parse_args()
a.mount=a.mount.resolve();a.output=a.output.resolve();assert not a.output.is_relative_to(a.mount);a.output.mkdir(exist_ok=False,parents=True)
fd=os.open(a.mount,os.O_PATH|os.O_DIRECTORY)
try:
 mid=next(x.split()[1] for x in Path('/proc/self/fdinfo/%d'%fd).read_text().splitlines() if x.startswith('mnt_id:'))
 assert next(x.split(' - ',1)[1].split()[0] for x in Path('/proc/self/mountinfo').read_text().splitlines() if x.split()[0]==mid)=='fuse.efs-fuse'
finally:os.close(fd)
root=None
for n in range(64):
 candidate=a.mount/('snapshot-pressure-%d-%d-%d'%(os.getpid(),time.time_ns(),n));candidate.mkdir()
 if (0 if candidate.stat().st_ino&1 else 2)==a.group:root=candidate;break
 candidate.rmdir()
assert root is not None
file=root/'payload';payload=b'pressure-payload'
with file.open('xb') as f:f.write(payload);f.flush();os.fsync(f.fileno())
assert (0 if file.stat().st_ino&1 else 2)==a.group
names=['user.pressure%d'%n for n in range(3)]
values=[hashlib.sha256(bytes([n])).digest()*128 for n in range(3)]
for name,value in zip(names,values):os.setxattr(file,name,value)
started=time.monotonic();seq=0;failures=0;maximum=0
with (a.output/'latency.jsonl').open('w',buffering=1) as log:
 while time.monotonic()-started<a.seconds:
  n=seq%3;value=hashlib.sha256(seq.to_bytes(8,'big')).digest()*128;t=time.monotonic();record=dict(sequence=seq,time_ns=time.time_ns(),group=a.group)
  try:
   os.setxattr(file,names[n],value);values[n]=value
   assert os.getxattr(file,names[n])==value
   record['result']='PASS'
  except Exception as e:record.update(result='FAIL',error=repr(e));failures+=1
  record['seconds']=time.monotonic()-t;maximum=max(maximum,record['seconds']);log.write(json.dumps(record)+'\n');seq+=1
for name,value in zip(names,values):assert os.getxattr(file,name)==value
assert file.read_bytes()==payload
file.unlink();root.rmdir()
r=dict(result='FAIL' if failures else 'PASS',group=a.group,updates=seq,seconds=time.monotonic()-started,failures=failures,maximum_seconds=maximum)
(a.output/'complete.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r),flush=True);raise SystemExit(bool(failures))
