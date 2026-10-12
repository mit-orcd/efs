import importlib.util,sys,os,time,json,builtins
from pathlib import Path
wrapper=Path(__file__).resolve();source=Path(os.environ['EFS_DIR_ORIGINAL_SUITE']);worker=sys.argv[1:2]==['--worker'];sys.argv=[str(wrapper)]+sys.argv[1:] if worker else sys.argv[1:]
spec=importlib.util.spec_from_file_location('efs_original_posix',source);suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite)
suite.TESTS[:]=[(n,f) for n,f in suite.TESTS if n in {'dir_readdir_listing','names_crazy_dirs'}];assert len(suite.TESTS)==2
root=os.environ['EFS_DIR_OBSERVER_ROOT'];suite.__file__=str(wrapper);dest=Path(os.environ['EFS_DIR_TRACE']);dest.mkdir(exist_ok=True)
rawopen=os.open;rawwrite=os.write;rawclose=os.close
state={'pid':0,'fd':-1,'seq':0}
def emit(row):
 if state['pid']!=os.getpid():
  if state['fd']>=0:rawclose(state['fd'])
  state.update(pid=os.getpid(),fd=rawopen(str(dest/('%d.jsonl'%os.getpid())),os.O_CREAT|os.O_WRONLY|os.O_APPEND,0o600),seq=0)
 try:rawwrite(state['fd'],(json.dumps(row)+'\n').encode())
 except OSError:pass
def call(op,path,fn,*args,**kwargs):
 state['seq']+=1;seq=state['seq'];start=time.monotonic_ns();emit({'event':'begin','seq':seq,'op':op,'path':str(path)[-200:],'ns':start})
 try:r=fn(*args,**kwargs)
 except BaseException as e:
  emit({'event':'end','seq':seq,'op':op,'ns':time.monotonic_ns(),'duration_ns':time.monotonic_ns()-start,'errno':getattr(e,'errno',None),'error':type(e).__name__});raise
 emit({'event':'end','seq':seq,'op':op,'ns':time.monotonic_ns(),'duration_ns':time.monotonic_ns()-start});return r
for n in ['mkdir','listdir','unlink','rmdir']:
 fn=getattr(os,n)
 def wrap(*a,_n=n,_f=fn,**k):return call(_n,a[0] if a else '',_f,*a,**k)
 setattr(os,n,wrap)
oldopen=builtins.open
class Observed:
 def __init__(self,f,path):self.f=f;self.path=path
 def __getattr__(self,n):return getattr(self.f,n)
 def write(self,*a,**k):return call('write',self.path,self.f.write,*a,**k)
 def read(self,*a,**k):return call('read',self.path,self.f.read,*a,**k)
 def close(self):return call('close',self.path,self.f.close)
 def __enter__(self):return self
 def __exit__(self,*a):return call('close-exit',self.path,self.f.__exit__,*a)
def opened(path,*a,**k):
 f=call('open',path,oldopen,path,*a,**k)
 return Observed(f,path) if str(path).startswith(root) else f
builtins.open=opened
for name,fn in list(suite.TESTS):
 def body(d,_fn=fn,_name=name):
  emit({'event':'inside-case','case':_name,'pid':os.getpid(),'ns':time.monotonic_ns()});return _fn(d)
 suite.TESTS[suite.TESTS.index((name,fn))]=(name,body)
suite.main()
