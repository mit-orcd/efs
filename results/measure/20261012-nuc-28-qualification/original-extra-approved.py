import os,sys,time,json,subprocess,signal,shutil,socket
from pathlib import Path
# Reuse the result recorder without dispatching its first matrix.
exec(Path('/tmp/amd-all-follow-approved.py').read_text().split('while not (out/')[0])
while not (out/'all-suites.finished').exists():time.sleep(10)
base.update(EFS_GIT_ID='28b2089b-release-normal',EFS_GIT_BRANCH='release-v0.3.0-pre-alpha',EFS_GIT_DESCRIBE='v0.3.0-pre-alpha-candidate')
fault=out/'fault-live-src'
def extra_gc(name,args,mode,variant='normal'):
 s=fault if variant=='fault' else src;work=out/'live-extra'/name;work.mkdir(parents=True,exist_ok=True)
 cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2',*args]
 if variant=='fault':cmd+=['--server-binary',s/'efsd','--client-binary',s/'efs-fuse']
 gate(name,cmd,timeout=1800,cwd=s)
gate('pack-soak-build',['make','-j8','EFS_GIT_ID=28b2089b-release-normal','efsd','efs-fuse','efs-mgmt','tests/stress/pack_storage_soak'])
gate('pack-soak',['tests/stress/pack_storage_soak','60','/tmp'],timeout=300)
for mode in ('direct','buffered'):
 work=out/'live-extra'/('cursor-replay-'+mode);work.mkdir(parents=True,exist_ok=True)
 gate('cursor-replay-'+mode,['python3','tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--applied-checkpoint-replay-only'],dict(EFS_TEST_CURSOR_CHECKPOINT='1'),timeout=1800)
 for option,parent in [('create-lease','create'),('namespace-boundary','namespace'),('namespace-max','namespace'),('append-replay-race','integrity'),('fresh-namespace','namespace')]:
  marker=out/'gates'/(parent+'-'+mode)/'result.json'
  if not marker.exists() or json.loads(marker.read_text())['returncode']!=0:extra_gc('extra-'+option+'-'+mode,['--'+option],mode)
 extra_gc('extra-full-'+mode,['--fold-audit','--workers','--pressure','--production-budgets','--integrity','--report-audit','--append-audit'],mode)
 extra_gc('overload-replication-'+mode,['--tiny-sustained-seconds','120','--tiny-overload','--replication-pressure'],mode,'fault')
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
    except subprocess.TimeoutExpired:p.kill();p.wait()
   for h in handles:h.close()
for m in ('/data1/efs/mnt','/data1/efs/mnt2'):
 gate('restore-'+Path(m).name,['/data1/efs/src/scripts/client.sh','127.0.0.1:17432',m])
(out/'extra-suites.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
