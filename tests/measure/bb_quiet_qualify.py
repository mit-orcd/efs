#!/usr/bin/env python3
"""One guarded base/candidate BB comparison after demonstrated foreign overlap.

Reads all-user process metadata using the existing tested Task-I guard. Never
signals foreign work. Quiet means no observed other EFS/test/build activity;
the identified resident intel-mnt client stays in place and is recorded. A
sampled guard is not an exclusive-host lease or proof of the timeout's cause.
"""
import argparse,hashlib,json,os,socket,subprocess,sys,threading,time,traceback
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
for n in ('private','source','base','output'):p.add_argument(n,type=Path)
p.add_argument('commit');p.add_argument('--background-client-pid',type=int,default=234552);a=p.parse_args();r=a.private.resolve();out=a.output.absolute()
assert r.name=='efs-taskBB-20261029' and r.stat().st_uid==os.getuid()
assert r in a.source.resolve().parents and r in a.base.resolve().parents and r in out.parents
assert not out.exists();out.mkdir();state=dict(verdict='WAITING_OTHER_WORKLOAD',source=a.commit,host=socket.gethostname(),commands=[])
sys.path.insert(0,str(a.source/'scripts'));from task_i_host_sample import foreign_jobs
assert socket.gethostname().split('.')[0]=='xorinox'
resident=a.background_client_pid;proc=Path('/proc')/str(resident)
expected=['./efs-fuse','10.1.0.208:17432','/data1/efs/intel-mnt']
def resident_identity():
 assert proc.stat().st_uid==1001
 args=(proc/'cmdline').read_bytes().decode().split('\0')
 assert args[:3]==expected,args
 f=(proc/'stat').read_text().rsplit(')',1)[1].split()
 return dict(pid=resident,start_ticks=int(f[19]),cpu_ticks=int(f[11])+int(f[12]),command=args[:3])
identity=resident_identity();state['background_client']=identity
log=(out/'host-samples.jsonl').open('x');lock=threading.Lock();stop=threading.Event();overlap=[]
def save():
 t=out/'state.tmp';t.write_text(json.dumps(state,indent=2)+'\n');t.replace(out/'state.json')
def sample():
 jobs=foreign_jobs(r);background=None
 try:
  background=resident_identity();assert background['start_ticks']==identity['start_ticks']
  jobs=[x for x in jobs if x['pid']!=resident]
 except (OSError,AssertionError,ValueError) as e:jobs.append(dict(reason='background identity changed',error=str(e)))
 row=dict(t=time.time(),foreign_jobs=jobs,background_client=background)
 with lock:log.write(json.dumps(row)+'\n');log.flush()
 return row
# No observer fsync during the workload. Any observed overlap/unknown after
# admission invalidates quiet qualification, even if all child commands pass.
def observe():
 while not stop.wait(10):
  try:
   row=sample()
   if row['foreign_jobs']:overlap.append(row)
  except BaseException as e:overlap.append(dict(reason='observer failed',error=repr(e)))
def run(name,argv,cwd,env,timeout):
 state['current']=name;save();started=time.monotonic();path=out/(name+'.log')
 with path.open('x') as f:
  f.write(json.dumps(dict(argv=list(map(str,argv)),cwd=str(cwd)))+'\n');f.flush()
  rc=subprocess.run(list(map(str,argv)),cwd=cwd,env=env,stdout=f,stderr=subprocess.STDOUT,timeout=timeout).returncode
 state['commands'].append(dict(name=name,rc=rc,elapsed_s=time.monotonic()-started,sha256=hashlib.sha256(path.read_bytes()).hexdigest()));save()
 assert rc in (0,3),(name,rc)
 return path.read_text(errors='replace')
thread=None
try:
 save();deadline=time.monotonic()+14400;quiet=None
 while True:
  row=sample();now=time.monotonic()
  quiet=(quiet if quiet is not None else now) if not row['foreign_jobs'] else None
  if quiet is not None and now-quiet>=60:break
  assert now<deadline,'NOTRUN: no 60-second quiet window within four hours'
  time.sleep(10)
 state['verdict']='RUNNING';state['quiet_start_sample']=row;save()
 thread=threading.Thread(target=observe,daemon=True);thread.start()
 env={k:v for k,v in os.environ.items() if not k.startswith('EFS_')}
 env.update(EFS_TRANSPORT='tcp',EFS_GIT_ID='e6856dd08',EFS_GIT_DESCRIBE='e6856dd08',EFS_GIT_BRANCH='agent/devel-bb')
 for name in ('base-a','base-b','base-tmp'):(out/name).mkdir()
 env['TMPDIR']=str(out/'base-tmp')
 run('base-version',[a.base/'efsd','--version'],a.base,env,60)
 text=run('base-gc-posix',['python3',a.base/'tests/live/gc_reclamation.py','--transport','tcp','--root',out/'base-a','--root',out/'base-b','--port','19954','--posix'],a.base,env,3600)
 assert 'GC reclamation PASS' in text and 'POSIX suite: 218/218 pass' in text and 'POSIX 2-client: 68/68 pass' in text,text[-3000:]
 assert not overlap,'foreign work observed during base comparison'
 driver=Path(__file__).with_name('bb_qualify.py')
 run('candidate',['python3','-u',driver,r,a.source,a.base,out/'candidate',a.commit,'--run-id',a.commit+'-quiet1'],a.source,env,10800)
 candidate=json.loads((out/'candidate/state.json').read_text())
 assert candidate['source']==a.commit and candidate['verdict'] in ('PASS_SCOPED','PASS_EXECUTED_WITH_HOST_LIMITS') and candidate['gate_rc'] in (0,3) and ' FAIL 0 ' in candidate['gate_summary']
 if sample()['foreign_jobs']:overlap.append(dict(reason='foreign work at final sample'))
 assert not overlap,'observed foreign work prevents quiet qualification'
 state['verdict']='PASS_SCOPED_OBSERVED_QUIET';state['candidate_state_sha256']=hashlib.sha256((out/'candidate/state.json').read_bytes()).hexdigest()
except BaseException:
 state.update(verdict='FAIL',error=traceback.format_exc());raise
finally:
 stop.set()
 if thread:thread.join(timeout=15)
 state['overlap']=overlap;save();log.close()
