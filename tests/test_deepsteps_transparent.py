#!/usr/bin/env python3
"""Actual diagnostic wrappers preserve descriptor arguments/results/exceptions."""
from pathlib import Path
import importlib.util,os,socket,sys,tempfile,subprocess
root=Path(__file__).resolve().parents[1]
source=Path(os.environ.get('EFS_DEEPSTEPS_SOURCE',root/'scripts/deepsteps_sitecustomize.py'))
names=('mkdir','stat','open','lstat','fstat','rmdir','unlink','scandir')
saved={n:getattr(os,n) for n in names}
capabilities={n:set(getattr(os,n)) for n in ('supports_fd','supports_dir_fd','supports_follow_symlinks','supports_effective_ids')}
try:
 spec=importlib.util.spec_from_file_location('deepsteps_checked',source)
 m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
 for capability,original_set in capabilities.items():
  for name,original in saved.items():assert (getattr(os,name) in getattr(os,capability))==(original in original_set),(name,capability)
finally:
 for n,f in saved.items():setattr(os,n,f)
 for n,values in capabilities.items():getattr(os,n).clear();getattr(os,n).update(values)
# Import the real shutil only after the observer in a fresh interpreter.
# Existing shutil imports can conceal capability regressions in a unit process.
base=subprocess.check_output([sys.executable,'-S','-c','import shutil;print(shutil.rmtree.avoids_symlink_attacks)'],text=True).strip()
probe='import sys; p=sys.argv[1];exec(compile(open(p).read(),p,"exec"),{"__name__":"startup_observer"});import shutil;print(shutil.rmtree.avoids_symlink_attacks)'
observed=subprocess.check_output([sys.executable,'-S','-c',probe,str(source)],text=True).strip()
assert observed==base,(base,observed)
events=[];calls=[];resolved=[];token=object()
m.address='controlled-sink';m._send_step=lambda *a:events.append(a)
def resolve(p):
 resolved.append(p)
 if p!='/proc/self/fd/7':raise OSError(9,'bad descriptor')
 return '/mnt/dir_deep_nesting/d00/d01'
m._original_readlink=resolve
def delegate(path,*args,**kw):calls.append((path,args,kw));return token
for op,path,args,kw,depth in [('rmdir','d02',(),{'dir_fd':7},3),('open',b'd02',(123,),{'dir_fd':7},3),('fstat',7,(),{},2),('scandir_open',7,(),{},2),('stat','/mnt/dir_deep_nesting/d00',(),{'follow_symlinks':False},1)]:
 events.clear();calls.clear();assert m._observe(delegate,op)(path,*args,**kw) is token
 assert calls==[(path,args,kw)] and len(events)==2
 assert events[0][:3]==(op,'enter',depth) and events[1][:3]==(op,'exit',depth)
# Neither failed attribution, unrelated paths nor inactive observation changes
# the delegate's arguments, return object or exception identity.
for path,kw in [('d02',{'dir_fd':8}),('/ordinary/path',{}),(Path('/mnt/dir_deep_nesting/d00'),{})]:
 events.clear();assert m._observe(delegate,'rmdir')(path,**kw) is token;assert not events
m.address=None;resolved.clear();assert m._observe(delegate,'rmdir')('d02',dir_fd=7) is token;assert not resolved
m.address='controlled-sink';events.clear();error=OSError(5,'controlled original failure')
def fail(*a,**kw):raise error
try:m._observe(fail,'rmdir')('d02',dir_fd=7);raise AssertionError('original error lost')
except OSError as seen:assert seen is error
assert events[1][:3]==('rmdir','error',3)
# Use the production emitter against a full/missing sink, and verify explicit
# close on every failure. No leaked diagnostic socket or observer stall.
spec=importlib.util.spec_from_file_location('deepsteps_emitter',source)
saved={n:getattr(os,n) for n in names}
try:
 e=importlib.util.module_from_spec(spec);spec.loader.exec_module(e)
finally:
 for n,f in saved.items():setattr(os,n,f)
 for n,values in capabilities.items():getattr(os,n).clear();getattr(os,n).update(values)
e.address='controlled-sink';e._original_readlink=resolve;closed=[]
class Full:
 def __enter__(self):return self
 def __exit__(self,*args):closed.append(True)
 def setblocking(self,b):assert b is False
 def sendto(self,*args):raise OSError(11,'observer queue full')
original_socket=socket.socket
try:
 socket.socket=lambda *a:Full()
 assert e._observe(delegate,'rmdir')('d02',dir_fd=7) is token
 assert len(closed)==2
finally:socket.socket=original_socket
if sys.platform=='linux':
 with tempfile.TemporaryDirectory(prefix='efs-deepsteps-') as d:
  p=Path(d)/'dir_deep_nesting'/'d00';p.mkdir(parents=True);(p/'d01').mkdir()
  fd=os.open(p,os.O_RDONLY);events.clear();m._original_readlink=os.readlink
  try:
   m._observe(os.rmdir,'rmdir')('d01',dir_fd=fd)
   assert not (p/'d01').exists() and events[0][:3]==('rmdir','enter',2)
  finally:os.close(fd)
print('deepsteps: descriptor-relative attribution, unchanged arguments/results/iterator and exception identity, quiet/missing/full-sink safety PASS')
