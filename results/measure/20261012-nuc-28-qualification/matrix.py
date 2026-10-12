import os,sys,time,json,re,hashlib,subprocess,signal,shutil,socket,traceback
from pathlib import Path
home=Path('/home/efs/nuc-28-qualification');src=home/'source-normal';fault=home/'source-fault';out=home/'results';os.chdir(src)
base=dict(os.environ,EFS_GIT_ID='28b2089b-release-normal',EFS_GIT_BRANCH='release-v0.3.0-pre-alpha',EFS_GIT_DESCRIBE='v0.3.0-pre-alpha-candidate',EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_RAFT_BATCH_OVERLAP='0',EFS_RAFT_BATCH_NOOP='0',EFS_PACKED_WRITES='0',EFS_TRANSPORT='tcp',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20',EFS_CLUSTER_SCRIPTS=str(out/'cluster-scripts'),EFS_TEST_WORK_ROOT=str(out/'private-work'))
(out/'private-work').mkdir(exist_ok=True)
ledger=out/'all-gates.jsonl'
active=None
def gate(name,cmd,env=None,timeout=1800,cwd=src):
 global active
 assert not (out/'gates'/name/'result.json').exists(),'do not silently retry prior verdict'
 if any('query_publication.py' in str(c) for c in cmd):cmd=list(cmd)+['--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2']
 d=out/'gates'/name;d.mkdir(parents=True,exist_ok=False);t=time.monotonic();e=base.copy();e.update(env or {})
 row=dict(name=name,command=list(map(str,cmd)),cwd=str(cwd),environment={k:v for k,v in e.items() if k.startswith('EFS_')},source='28b2089be6a5df835cc7bb46b9c2a9e2f10df190',started=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),timeout=timeout)
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
 s=fault if variant=='fault' else src;work=out/'live-extra'/name;work.mkdir(parents=True,exist_ok=False)
 cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2',*args]
 if variant=='fault':cmd+=['--server-binary',s/'efsd','--client-binary',s/'efs-fuse']
 gate(name,cmd,timeout=1800,cwd=s)

def matrix():
 gate('pack-soak',['tests/stress/pack_storage_soak','60','/tmp'],timeout=300)
 for mode in ('direct','buffered'):
  work=out/'live-extra'/('cursor-replay-'+mode);work.mkdir(parents=True,exist_ok=True)
  gate('cursor-replay-'+mode,['python3','tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--applied-checkpoint-replay-only'],dict(EFS_TEST_CURSOR_CHECKPOINT='1'),timeout=1800)
  # Parent combination coverage: direct confirmed by AMD; buffered conditional pass deferred to separate final stage.
  extra_gc('extra-full-'+mode,['--fold-audit','--workers','--pressure','--production-budgets','--integrity','--report-audit','--append-audit'],mode)
  gate('query-load-noexec-'+mode,['python3','tests/live/query_publication.py',Path('/tmp')/('efs-all-28b2089b-query-load-noexec-'+mode),'--mode',mode],dict(EFS_QUERY_RPC='1',EFS_QUERY_FUSE='1',EFS_QUERY_LOAD='1',EFS_QUERY_NOEXEC='1'),timeout=1800)
 
 gate('fuse-workers',['python3','tests/test_fuse_workers.py'])
 gate('get-pair-deadline',['python3','tests/live/get_pair_deadline.py'])
 probe=out/'compaction-pressure-probe'
 if gate('production-pressure-probe-build',['cc','-std=gnu11','-O2','-g','-I'+str(fault/'include'),str(fault/'tests/live/compaction_pressure.c'),str(fault/'libefs.a'),'-lpthread','-lm','-ldl','-libverbs','-o',probe])==0:
  gate('production-cap-pressure',['python3',fault/'tests/live/compaction_pressure.py',out/'live-extra/production-cap-pressure','--probe',probe,'--mount','/data1/efs/mnt'],timeout=1800,cwd=fault)
 for mode in ('direct','buffered'):
  gate('query-policy-tmp-'+mode,['python3','tests/live/query_publication.py',Path('/tmp')/('efs-all-28b2089b-query-policy-'+mode),'--mode',mode],dict(EFS_QUERY_RPC='1',EFS_QUERY_FUSE='1',EFS_QUERY_POLICY='1',EFS_QUERY_FAILOVER='1',EFS_QUERY_EXTRAS='1',EFS_QUERY_VIRTUAL='1'),timeout=1800)
 for name in ('fragment_integrity','server_shutdown'):gate(name,['python3','tests/live/'+name+'.py'])
 for mode in ('direct','buffered'):
  for name in ('raft_read_freshness','checkpoint_sync_fault','io_stats_smoke'):
   gate(name+'-'+mode,['python3','tests/live/'+name+'.py','--mode',mode],timeout=1800)
  for apply in (False,True):
   n='publication-pending-'+mode+('-apply' if apply else '')
   gate(n,['python3',fault/'tests/live/publication_pending.py',out/'live-extra'/n,'--mode',mode,*(['--apply-pending'] if apply else [])],cwd=fault)
  for cross in (False,True):
   n='publication-replay-'+mode+('-cross' if cross else '');m=Path('/data1/efs/mnt')
   gate(n,['python3',fault/'tests/live/publication_replay.py',out/'live-extra'/n,'--mount',m,'--mode',mode,*(['--cross-group'] if cross else [])],cwd=fault)
  for engine in ('files','containers'):
   name='persist-engine-'+engine+'-'+mode;work=out/'live-extra'/name;work.mkdir(parents=True,exist_ok=True);mnt=Path('/data1/efs/mnt');procs=[];handles=[]
   env=base.copy();env['EFS_MD_RAFT_N']='4';seed='127.0.0.1:27500'
   try:
    for i in range(4):
     log=(work/('n'+str(i+1)+'.log')).open('w');handles.append(log)
     cmd=[str(src/'efsd'),'--node-id',str(i+1),'--addr','127.0.0.1','--port',str(27500+i),'--storage',str(work/('n'+str(i+1))),'--quota','2G','--storage-engine',engine,'--direct-io' if mode=='direct' else '--no-direct-io']
     if i:cmd+=['--join',seed]
     p=subprocess.Popen(cmd,env=env,stdout=log,stderr=log);procs.append(p)
     end=time.monotonic()+30
     while time.monotonic()<end:
      if p.poll() is not None:raise RuntimeError('daemon exited')
      if 'listening' in (work/('n'+str(i+1)+'.log')).read_text():break
      time.sleep(.2)
     else:raise RuntimeError('daemon start timeout')
    if gate(name+'-mkfs',[src/'efs-mgmt','raft-mkfs',seed]):raise RuntimeError('mkfs failed')
    if gate(name+'-mount',[src/'scripts/client.sh',seed,mnt]):raise RuntimeError('mount failed')
    if gate(name+'-prepare',['python3','tests/posix/posix_persist.py',mnt,'--phase','prepare','--tag',name,'--keep','--results',work/'prepare.tsv']):raise RuntimeError('prepare failed; verify blocked')
    if gate(name+'-unmount',[src/'scripts/client.sh','stop',mnt]):raise RuntimeError('unmount failed')
    if gate(name+'-remount',[src/'scripts/client.sh',seed,mnt]):raise RuntimeError('remount failed')
    gate(name+'-verify',['python3','tests/posix/posix_persist.py',mnt,'--phase','verify','--tag',name,'--results',work/'verify.tsv'])
    for phase in ('create','read','churn','verify','delete'):
     if gate(name+'-ab-'+phase,['python3','tests/live/storage_engine_ab.py',phase,'--mount',mnt,'--state',work/'ab-state.json','--output',work/('ab-'+phase+'.json')]):break
    if engine=='containers':
     gate(name+'-core',['python3','tests/posix/posix_suite.py',mnt,'--jobs','4','--timeout-s','30','--results',work/'core.tsv'])
     peer=Path('/data1/efs/mnt2')
     if gate(name+'-peer-mount',[src/'scripts/client.sh',seed,peer])==0:
      gate(name+'-peer',['python3','tests/posix/posix_2client.py','--local',mnt,peer,'--parent','containers-peer','--results',work/'peer.tsv'])
      gate(name+'-peer-stop',[src/'scripts/client.sh','stop',peer])
    gate(name+'-extras',['python3','tests/posix/posix_extras.py',mnt,'--results',work/'extras.tsv'])
   except Exception as e:
    (work/'blocked.json').write_text(json.dumps({'status':'BLOCKED','reason':str(e)},indent=2));print(name,'BLOCKED',e,flush=True)
   finally:
    for target in (mnt,Path('/data1/efs/mnt2')):
     if os.path.ismount(target):gate(name+'-cleanup-'+target.name,[src/'scripts/client.sh','stop',target])
    for p in procs:
     if p.poll() is None:p.terminate()
    for p in procs:
     try:p.wait(timeout=30)
     except subprocess.TimeoutExpired:raise RuntimeError('private daemon did not stop; retain state, no force kill')
    for h in handles:h.close()
 (out/'extra-suites.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')

if __name__=="__main__":matrix()
