import importlib.util,json,os,sys,time
from pathlib import Path
source=Path(sys.argv[1]);sys.argv=sys.argv[1:]
# Observe the original selected case; its assertions/decorators remain intact.
spec=importlib.util.spec_from_file_location('efs_original_posix',source)
suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
suite.TESTS[:]=[(n,f) for n,f in suite.TESTS if n=='dir_deep_nesting']
assert len(suite.TESTS)==1
root=os.environ['EFS_DEEP_TRACE_ROOT'];trace=Path(os.environ['EFS_DEEP_TRACE_DIR']);trace.mkdir(exist_ok=True)
raw_open=os.open;raw_write=os.write;raw_close=os.close
state={'pid':0,'fd':-1,'count':0};limit=8000
def emit(row):
 pid=os.getpid()
 if state['pid']!=pid:
  if state['fd']>=0:raw_close(state['fd'])
  state.update(pid=pid,fd=raw_open(str(trace/('%d.jsonl'%pid)),os.O_WRONLY|os.O_CREAT|os.O_APPEND,0o600),count=0)
 try:raw_write(state['fd'],(json.dumps(row,separators=(',',':'))+'\n').encode())
 except OSError:pass
def observe(name,fn):
 def wrapped(*args,**kwargs):
  path=args[0] if args else None
  try:path=os.fspath(path)
  except TypeError:path=('fd:%d'%args[0]) if args and isinstance(args[0],int) else None
  if isinstance(path,bytes):path=path.decode(errors='replace')
  chosen=path is not None and (not os.path.isabs(path) or path.startswith(root))
  if not chosen or state['count']>=limit:return fn(*args,**kwargs)
  state['count']+=1;start=time.monotonic_ns();row={'op':name,'pid':os.getpid(),'start_ns':start,'path':path[-240:],'path_depth':path.count('/')}
  emit(dict(row,event='begin'))
  try:
   result=fn(*args,**kwargs)
  except BaseException as error:
   emit(dict(row,event='end',elapsed_ns=time.monotonic_ns()-start,error=type(error).__name__,errno=getattr(error,'errno',None)));raise
  emit(dict(row,event='end',elapsed_ns=time.monotonic_ns()-start));return result
 return wrapped
for name in ('mkdir','stat','lstat','open','scandir','rmdir','unlink'):
 setattr(os,name,observe(name,getattr(os,name)))
phase=suite._deep_phase
def traced_phase(event,worker=0):
 emit({'event':'phase','phase':event,'pid':os.getpid(),'worker':worker,'mono_ns':time.monotonic_ns()});return phase(event,worker)
suite._deep_phase=traced_phase
suite.main()
