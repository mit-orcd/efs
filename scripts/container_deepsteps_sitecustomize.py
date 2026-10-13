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
def _send_step(op,event,depth,elapsed=0):
 try:
  emitter=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
  emitter.setblocking(False)  # A full observer queue must not stall the case.
  emitter.sendto(('deepstep op=%s event=%s depth=%d pid=%d mono_ns=%d elapsed_ns=%d\n'%(op,event,depth,os.getpid(),time.monotonic_ns(),elapsed)).encode(),address)
  emitter.close()
 except OSError:pass
def _observe(original,op):
 def wrapped(path,*args,**kwargs):
  if isinstance(path,bytes):text_path=os.fsdecode(path)
  elif isinstance(path,str):text_path=path
  else:return original(path,*args,**kwargs)
  if not address or '/dir_deep_nesting/' not in text_path:return original(path,*args,**kwargs)
  depth=sum(bool(re.fullmatch('d[0-9]{2}',part)) for part in text_path.split('/'))
  if not depth:return original(path,*args,**kwargs)
  started=time.monotonic_ns();_send_step(op,'enter',depth)
  try:result=original(path,*args,**kwargs)
  except BaseException:
   _send_step(op,'error',depth,time.monotonic_ns()-started);raise
  _send_step(op,'exit',depth,time.monotonic_ns()-started);return result
 return wrapped
os.mkdir=_observe(_original_mkdir,'mkdir')
os.stat=_observe(_original_stat,'stat')
