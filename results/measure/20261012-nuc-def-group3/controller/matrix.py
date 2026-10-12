import os,sys,time,json,re,hashlib,subprocess,signal,shutil,socket,traceback
from pathlib import Path
home=Path('/home/efs/nuc-def-group3');src=home/'source-normal';fault=home/'source-fault';out=home/'results';os.chdir(src)
base=dict(os.environ,EFS_GIT_ID='def46109-group3-normal',EFS_GIT_BRANCH='release-v0.3.0-pre-alpha',EFS_GIT_DESCRIBE='v0.3.0-pre-alpha-candidate',EFS_REPAIR_EXPERIMENT='0',EFS_REPAIR_INIT_STORE='0',EFS_STORAGE_ENGINE='files',EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_PACKED_WRITES='0',EFS_TRANSPORT='tcp',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20',EFS_CLUSTER_SCRIPTS=str(out/'cluster-scripts'),EFS_TEST_WORK_ROOT=str(out/'private-work'))
(out/'private-work').mkdir(exist_ok=True)
ledger=out/'all-gates.jsonl'
active=None
def gate(name,cmd,env=None,timeout=1800,cwd=src):
 global active
 assert not (out/'gates'/name/'result.json').exists(),'do not silently retry prior verdict'
 if any('query_publication.py' in str(c) for c in cmd):cmd=list(cmd)+['--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2']
 d=out/'gates'/name;d.mkdir(parents=True,exist_ok=False);t=time.monotonic();e=base.copy();e.update(env or {})
 row=dict(name=name,command=list(map(str,cmd)),cwd=str(cwd),environment={k:v for k,v in e.items() if k.startswith('EFS_')},source='def46109909c793e58ae2c453c5de537cdb2c627',started=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),timeout=timeout)
 (d/'command.json').write_text(json.dumps(row,indent=2));print(row['started'],name,'START',flush=True)
 with (d/'run.log').open('w') as log:
  p=subprocess.Popen(row['command'],cwd=cwd,env=e,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  active=p
  (d/'pid.txt').write_text(str(p.pid))
  try:rc=p.wait(timeout=timeout)
  except subprocess.TimeoutExpired:
   p.terminate()
   try:p.wait(timeout=30)
   except subprocess.TimeoutExpired:raise RuntimeError('deadline cleanup did not finish; no forced discard')
   rc=124
 active=None
 row.update(returncode=rc,seconds=round(time.monotonic()-t,3),status='PASS' if rc==0 else 'NOTRUN' if rc==77 else 'FAIL')
 (d/'result.json').write_text(json.dumps(row,indent=2));(d/'run.exit').write_text(str(rc)+'\n')
 with ledger.open('a') as f:f.write(json.dumps(row)+'\n')
 print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),name,row['status'],rc,flush=True)
 assert not any(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')) if not name.startswith('persist-engine-') else True,'private mount retained; stop queue'
 return rc

def extra_gc(name,args,mode,variant='normal'):
 s=fault if variant=='fault' else src;work=Path('/data1/efs/nuc-def-group3')/name;work.mkdir(parents=True,exist_ok=False)
 cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2',*args]
 if variant=='fault':cmd+=['--server-binary',s/'efsd','--client-binary',s/'efs-fuse']
 gate(name,cmd,timeout=1800,cwd=s)

def matrix():
 extra_gc('posix-core-peer-direct',['--posix'], 'direct')
 extra_gc('mounted-packing-direct',['--mounted-packing-only'], 'direct')
 extra_gc('mounted-packing-seven-quiet',['--mounted-packing-only','--posix','--packing-clients','7','--posix-jobs','16','--fixture-quota','4G'], 'direct')
