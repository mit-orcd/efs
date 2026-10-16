"""Diagnostic-only transparent observers for original POSIX deep path calls.

Copy as sitecustomize.py into a private observer directory and prepend that
one directory to PYTHONPATH of the phase-socket diagnostic harness. Fresh
worker processes inherit it; the normal POSIX suite and calls are unchanged.
Never use these timings as profiling-OFF release acceptance.
"""
import os,socket
address=os.environ.get('EFS_POSIX_PHASE_SOCKET')
import time,re
_original_mkdir=os.mkdir
_original_stat=os.stat
_original_readlink=os.readlink
def _send_step(op,event,depth,elapsed=0):
 try:
  with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as emitter:
   emitter.setblocking(False)  # A full observer queue must not stall the case.
   emitter.sendto(('deepstep op=%s event=%s depth=%d pid=%d mono_ns=%d elapsed_ns=%d\n'%(op,event,depth,os.getpid(),time.monotonic_ns(),elapsed)).encode(),address)
 except OSError:pass
def _diagnostic_path(path,kwargs):
 # Resolve descriptor-relative cleanup names using local procfs only. Never
 # replace the original argument or iterator, nor query EFS to resolve it.
 try:
  if isinstance(path,int):return _original_readlink('/proc/self/fd/%d'%path)
  if isinstance(path,bytes):text_path=os.fsdecode(path)
  elif isinstance(path,str):text_path=path
  else:return None
  if not os.path.isabs(text_path) and isinstance(kwargs.get('dir_fd'),int):
   parent=_original_readlink('/proc/self/fd/%d'%kwargs['dir_fd'])
   return os.path.join(parent,text_path)
  return text_path
 except OSError:return None

def _observe(original,op):
 def wrapped(path,*args,**kwargs):
  if not address:return original(path,*args,**kwargs)
  text_path=_diagnostic_path(path,kwargs)
  if not text_path or '/dir_deep_nesting/' not in text_path:return original(path,*args,**kwargs)
  depth=sum(bool(re.fullmatch('d[0-9]{2}',part)) for part in text_path.split('/'))
  if not depth:return original(path,*args,**kwargs)
  started=time.monotonic_ns();_send_step(op,'enter',depth)
  try:result=original(path,*args,**kwargs)
  except BaseException:
   _send_step(op,'error',depth,time.monotonic_ns()-started);raise
  _send_step(op,'exit',depth,time.monotonic_ns()-started);return result
 return wrapped
def _install(name,op):
 original=getattr(os,name);wrapped=_observe(original,op)
 # Python chooses descriptor-safe shutil.rmtree through these identity
 # sets. Preserve every declared capability of the wrapped OS operation.
 for capability in ('supports_fd','supports_dir_fd','supports_follow_symlinks','supports_effective_ids'):
  supported=getattr(os,capability,set())
  if original in supported:supported.add(wrapped)
 setattr(os,name,wrapped)
_install('mkdir','mkdir')
_install('stat','stat')
for _name in ('open','lstat','fstat','rmdir','unlink'):_install(_name,_name)
# This times the constructor only; leave the native iterator unchanged.
_install('scandir','scandir_open')
