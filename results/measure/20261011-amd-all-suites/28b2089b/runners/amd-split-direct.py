import os,sys,time,json,re,subprocess,shutil
from pathlib import Path
src=Path('/data1/efs/release-v030-28b2089b');out=Path('/data1/efs/release-v030-results-20261012-28b2089b');os.chdir(src)
base=dict(os.environ,EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_TRANSPORT='tcp',EFS_RDMA_DEV='rxe0',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20',EFS_CLUSTER_SCRIPTS=str(out/'cluster-scripts'))
ledger=out/'all-gates.jsonl'
def gate(name,cmd,env=None,timeout=1800,cwd=src):
 if not (name.endswith('-direct') or name=='rdma-posix'):return 0 # assigned to Intel/NUC or already completed
 old=out/'gates'/name/'result.json'
 if old.exists():
  previous=json.loads(old.read_text())
  return previous['returncode'] # retain existing failures; split is not a retry
  name+='-approved'
 if any('query_publication.py' in str(c) for c in cmd):cmd=list(cmd)+['--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2']
 d=out/'gates'/name;d.mkdir(parents=True,exist_ok=True);t=time.monotonic();e=base.copy();e.update(env or {})
 row=dict(name=name,command=list(map(str,cmd)),source='28b2089be6a5df835cc7bb46b9c2a9e2f10df190',started=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
 (d/'command.json').write_text(json.dumps(row,indent=2));print(row['started'],name,'START',flush=True)
 with (d/'run.log').open('w') as log:
  try:
   p=subprocess.Popen(row['command'],cwd=cwd,env=e,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
   try:rc=p.wait(timeout=timeout)
   except subprocess.TimeoutExpired:
    import signal
    os.killpg(p.pid,signal.SIGTERM)
    try:p.wait(timeout=30)
    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
    rc=124
  except Exception as ex:log.write(repr(ex));rc=125
 row.update(returncode=rc,seconds=round(time.monotonic()-t,3),status='PASS' if rc==0 else 'NOTRUN' if rc==77 else 'FAIL')
 (d/'result.json').write_text(json.dumps(row,indent=2));(d/'run.exit').write_text(str(rc)+'\n')
 with ledger.open('a') as f:f.write(json.dumps(row)+'\n')
 print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),name,row['status'],rc,flush=True);return rc
while not (out/'build-and-gates.exit').exists():time.sleep(10)
(out/'cluster-scripts').mkdir(exist_ok=True)
subprocess.run(['tar','-xzf','/tmp/amd-cluster-test-scripts.tar.gz','-C',str(out/'cluster-scripts')],check=True)
# Each standalone regression has its own result, even if a preceding gate failed.
make=(src/'Makefile').read_text();called=set(re.findall(r'tests/(test_[\w]+\.py)',make));standalone=[p for p in sorted((src/'tests').glob('test_*.py')) if p.name not in called]
(out/'standalone-inventory.json').write_text(json.dumps([p.name for p in standalone],indent=2))
for mode in ('normal','fault'):
 for p in standalone:gate('standalone-'+mode+'-'+p.stem,['python3',p],{'EFS_FAULTS':'1' if mode=='fault' else '0'},timeout=300)
gate('integrated-create-asan',['python3','tests/faults/test_create_retry_integrated.py'],{'CC':'cc -fsanitize=address,undefined -fno-omit-frame-pointer'})
gate('fault-create-asan',['python3','tests/faults/test_create_retry_fault.py'],{'EFS_SANITIZE':'1'})
gate('fault-simulator-build',['make','tests/faults/fault_sim'])
gate('fault-simulator',['bash','tests/faults/run_faults.sh','/tmp/efs-all-28b2089b-simulator'])
# Preserve full variant identity across all three live binaries.
fault=out/'fault-live-src';fault.mkdir(exist_ok=True)
for n in ('tests','scripts','src','include'):
 shutil.copytree(src/n,fault/n,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.o','__pycache__'))
for n in ('efsd','efs-fuse','efs-mgmt'):shutil.copy2(out/'fault-binaries'/n,fault/n)
# Core/peer/extras, packing, GC, query, cold recovery. Every fixture is private.
def gc(name,args,mode,variant='normal',timeout=1800):
 s=fault if variant=='fault' else src;work=out/'live-approved'/name;work.mkdir(parents=True,exist_ok=True)
 cmd=['python3','-u',s/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2',*args]
 if variant=='fault':cmd+=['--server-binary',s/'efsd','--client-binary',s/'efs-fuse']
 return gate(name,cmd,timeout=timeout,cwd=s)
for mode in ('direct','buffered'):
 gc('gc-posix-'+mode,['--posix','--continue-posix-failure'],mode)
 gc('packing-seven-'+mode,['--mounted-packing-only','--packing-clients','7','--posix-jobs','16','--posix','--continue-posix-failure','--fixture-quota','4G'],mode)
 gc('ordinary-overwrite-'+mode,['--ordinary-overwrite-only'],mode)
 for suffix,env in [('query',dict(EFS_QUERY_RPC='1',EFS_QUERY_FUSE='1',EFS_QUERY_POLICY='1',EFS_QUERY_FAILOVER='1',EFS_QUERY_EXTRAS='1',EFS_QUERY_VIRTUAL='1')),('large-delta',dict(EFS_QUERY_RPC='1',EFS_QUERY_FUSE='1',EFS_QUERY_LARGE_DELTA='1'))]:
  gate(suffix+'-'+mode,['python3','-u','tests/live/query_publication.py',out/'live-approved'/(suffix+'-'+mode),'--mode',mode],env,timeout=1800)
 for option in ('peer-mkdir-race-only','peer-open-contention-only','append-truncate-only','mount-sync','namespace-session-only','applied-checkpoint-replay-only','tiny-create-only','tiny-mixed-only','append-loaded-only','efs-commands-only'):
  gc(option+'-'+mode,['--'+option],mode)
 for name,args in [('append-burst',['--append-burst-only','--append-burst','1000']),('namespace',['--namespace','--namespace-boundary','--namespace-cycles','--namespace-max','--fresh-namespace']),('integrity',['--integrity','--tiny-churn','--sync-writes','--append-sync-boundary','--append-replay-race']),('cache',['--memo','--rename-cache','--mixed-reads','--allocation','--du-audit','--sequential']),('sessions',['--session-append','--session-cleanup','--deferred-close','--typed-writeback','--guarded-leases']),('create',['--unique-create','--create-lease','--create-negative-replay']),('shutdown',['--shutdown-mutations','--wal-tail-recovery']),('opid-restart',['--opid','--opid-restart']),('opid-failover',['--opid','--opid-failover']),('full-node',['--full-node']),('two-roots',['--storage-roots','2'])]:
  gc(name+'-'+mode,args,mode)
 for name,args in [('create-retry',['--create-live-retry']),('create-adopt-negative',['--create-adopt-negative']),('create-adopt-reply-loss',['--create-adopt-reply-loss']),('fuse-reply-failure',['--fuse-reply-failure']),('put-barrier',['--put-barrier-fault']),('kv-wal',['--kv-wal-persistence-only']),('transaction-apply',['--transaction-apply-retry']),('compactor',['--compactor-pressure']),('lookup',['--lookup-barrier']),('put-reply',['--put-reply-fault']),('storage-session-fence',['--storage-session-fence-fault','--guarded-leases']),('publication-pressure',['--publication-pressure','--pressure']),('drain-pressure',['--publication-pressure','--pressure','--drain-pressure']),('sync-write-fault',['--sync-write-fault']),('stop-dirty',['--stop-dirty-recovery']),('namespace-boundary-fault',['--namespace','--namespace-boundary','--namespace-boundary-fault']),('namespace-max-fault',['--namespace-max','--namespace-max-fault'])]:
  gc(name+'-'+mode,args,mode,'fault')
 for kind in ('allocation','tls','growth'):gc('worker-'+kind+'-'+mode,['--workers','--worker-fault',kind],mode,'fault')
 gate('fault-integration-'+mode,['bash','tests/faults/run_faults.sh','/tmp/efs-all-28b2089b-fault-'+mode,'--integrate','--mode',mode],{'EFS_FAULT_BIN':str(fault)},timeout=1800)
 gate('valgrind-'+mode,['bash','tests/valgrind_leaks.sh','/tmp/efs-all-28b2089b-vg-'+mode],{'EFS_VG_MODE':mode,'EFS_VG_PORT':'26320'},timeout=3600)
# Same-host RXE functional coverage (not physical GPU/verbs performance proof).
Path('/tmp/efs-rdma-28b2089b').mkdir(exist_ok=True)
gate('rdma-posix',['bash','tests/live/rdma_posix_gates.sh'],{'EFS_TRANSPORT':'rdma','EFS_GATE_ROOT':'/tmp/efs-rdma-28b2089b'},timeout=1800)
(out/'all-suites.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
