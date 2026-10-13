#!/usr/bin/env python3
"""Measure each foreground syscall, validate bytes, and retain tail latencies.

One thread, with optional query subprocesses only after all file I/O completes.
Results must be outside the mounted filesystem. No timeout or profile changes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('mount',type=Path)
parser.add_argument('output',type=Path)
parser.add_argument('--seconds',type=float,default=600)
parser.add_argument('--period',type=float,default=1)
parser.add_argument('--max-operation-seconds',type=float,default=1)
parser.add_argument('--query-root',type=Path)
parser.add_argument('--queries-every',type=int,default=30)
a=parser.parse_args();a.mount=a.mount.resolve();a.output=a.output.resolve()
assert a.seconds>0 and a.period>=0 and a.max_operation_seconds>0
assert not a.output.is_relative_to(a.mount)
a.output.mkdir(parents=True,exist_ok=False)
fd=os.open(a.mount,os.O_PATH|os.O_DIRECTORY)
try:
 mid=next(line.split()[1] for line in Path('/proc/self/fdinfo/%d'%fd).read_text().splitlines() if line.startswith('mnt_id:'))
 fs=next(line.split(' - ',1)[1].split()[0] for line in Path('/proc/self/mountinfo').read_text().splitlines() if line.split()[0]==mid)
 assert fs=='fuse.efs-fuse','refuse local/NFS fallback'
finally:os.close(fd)
root=a.mount/('foreground-latency-%d-%d'%(os.getpid(),time.time_ns()));root.mkdir()
started=time.monotonic();samples=[];failures=[];sequence=0
with (a.output/'latency.jsonl').open('w',buffering=1) as log:
 while time.monotonic()-started<a.seconds:
  cycle=time.monotonic();target=root/('file-%09d'%sequence);fd=None
  record=dict(sequence=sequence,time_ns=time.time_ns(),operations={},result='PASS')
  payload=hashlib.sha256(sequence.to_bytes(8,'big')).digest()*16
  def measured(name,operation):
   t=time.monotonic()
   try:return operation()
   finally:record['operations'][name]=time.monotonic()-t
  try:
   fd=measured('create',lambda:os.open(target,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600))
   def write_all():
    remaining=memoryview(payload)
    while remaining:
     n=os.write(fd,remaining)
     assert n>0,'write made no progress'
     remaining=remaining[n:]
   measured('write',write_all)
   measured('fsync',lambda:os.fsync(fd))
   closing_fd=fd;fd=None
   measured('close',lambda:os.close(closing_fd))
   t=time.monotonic()
   with target.open('rb',buffering=0) as stream:actual=stream.read()
   if actual!=payload:
    stem='payload-failure-%09d'%sequence
    (a.output/(stem+'-expected.bin')).write_bytes(payload)
    (a.output/(stem+'-actual.bin')).write_bytes(actual)
    record.update(path=str(target),expected_bytes=len(payload),actual_bytes=len(actual),
                  expected_sha256=hashlib.sha256(payload).hexdigest(),
                  actual_sha256=hashlib.sha256(actual).hexdigest())
    raise AssertionError('payload mismatch; exact bytes retained outside mount')
   record['verify_seconds']=time.monotonic()-t
   measured('unlink',target.unlink)
   record['combined_seconds']=sum(record['operations'].values())
   if any(v>a.max_operation_seconds for v in record['operations'].values()):
    record['result']='SLOW';failures.append(record)
  except BaseException as error:
   record.update(result='FAIL',error=repr(error));failures.append(record)
  finally:
   if fd is not None:os.close(fd)
  log.write(json.dumps(record)+'\n');samples.append(record)
  if a.query_root and sequence%a.queries_every==0:
   for name in ['stats','find']:
    command=[sys.executable,str(a.query_root/'.efs'/name),'--json']
    if name=='find':command+=[ 'file-','--match','prefix','--limit','100']
    t=time.monotonic();query=dict(sequence=sequence,operation=name,time_ns=time.time_ns())
    try:
     reply=subprocess.run(command,capture_output=True,timeout=30)
     query.update(seconds=time.monotonic()-t,exit=reply.returncode)
     if reply.returncode:raise RuntimeError(reply.stderr.decode(errors='replace'))
     value=json.loads(reply.stdout);query.update(result='PASS',snapshot=value.get('snapshot'))
    except Exception as error:
     query.update(result='FAIL',seconds=time.monotonic()-t,error=repr(error));failures.append(query)
    log.write(json.dumps(query)+'\n')
  sequence+=1
  time.sleep(max(0,a.period-(time.monotonic()-cycle)))
if not any(root.iterdir()):root.rmdir()
summary=dict(result='FAIL' if failures else 'PASS',samples=len(samples),seconds=time.monotonic()-started,max_operation_seconds=a.max_operation_seconds,failures=failures)
(a.output/'complete.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)
raise SystemExit(bool(failures))
