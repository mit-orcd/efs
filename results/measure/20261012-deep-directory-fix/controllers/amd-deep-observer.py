import importlib.util,os,sys,time,json
from pathlib import Path
wrapper=Path(__file__).resolve();worker=sys.argv[1:2]==['--worker'];sys.argv=[str(wrapper)]+sys.argv[1:] if worker else sys.argv[1:]
spec=importlib.util.spec_from_file_location('efs_original_posix',os.environ['EFS_DEEP_ORIGINAL']);suite=importlib.util.module_from_spec(spec);spec.loader.exec_module(suite);suite.__file__=str(wrapper)
dest=Path(os.environ['EFS_DEEP_TRACE']);dest.mkdir(exist_ok=True);active=False;state={'pid':0,'fd':-1};rawopen=os.open;rawwrite=os.write

def emit(row):
 if state['pid']!=os.getpid():state.update(pid=os.getpid(),fd=rawopen(str(dest/('%d.jsonl'%os.getpid())),os.O_CREAT|os.O_WRONLY|os.O_APPEND,0o600))
 rawwrite(state['fd'],(json.dumps(row)+'\n').encode())

def phase(event,worker=0):emit({'phase':event,'pid':os.getpid(),'ns':time.monotonic_ns()})
suite._deep_phase=phase
for name in ['stat','lstat','mkdir','rmdir','scandir']:
 original=getattr(os,name)
 def traced(*args,_name=name,_original=original,**kwargs):
  if not active:return _original(*args,**kwargs)
  start=time.monotonic_ns();emit({'begin':_name,'path':str(args[0])[-350:],'ns':start})
  try:return _original(*args,**kwargs)
  finally:emit({'end':_name,'path':str(args[0])[-350:],'ns':time.monotonic_ns(),'us':(time.monotonic_ns()-start)//1000})
 setattr(os,name,traced)
for i,(name,fn) in enumerate(suite.TESTS):
 if name=='dir_deep_nesting':
  def deep(d,_fn=fn):
   global active
   active=True
   try:return _fn(d)
   finally:active=False
  deep.__name__=fn.__name__;suite.TESTS[i]=(name,deep)
suite.main()
