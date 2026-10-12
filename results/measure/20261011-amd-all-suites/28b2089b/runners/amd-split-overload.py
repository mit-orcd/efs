import os,sys,time,json,re,subprocess,shutil
from pathlib import Path
src=Path('/data1/efs/release-v030-28b2089b');out=Path('/data1/efs/release-v030-results-20261012-28b2089b');os.chdir(src)
base=dict(os.environ,EFS_RAFT_FLOW='0',EFS_RAFT_FLOW_EARLY_RETIRE='0',EFS_RAFT_GROUP_DRAIN='0',EFS_TRANSPORT='tcp',EFS_RDMA_DEV='rxe0',EFS_DATA_GC_ADMISSION='1',EFS_GC_BACKLOG_IDLE_MS='20',EFS_CLUSTER_SCRIPTS=str(out/'cluster-scripts'))
ledger=out/'all-gates.jsonl'
def gate(name,cmd,env=None,timeout=1800,cwd=src):
 old=out/'gates'/name/'result.json'
 if old.exists():
  previous=json.loads(old.read_text())
  if previous['returncode']==0:return 0
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

while not (out/'all-suites.finished').exists():time.sleep(10)
fault=out/'fault-live-src'
for mode in ('direct','buffered'):
 name='overload-replication-'+mode;work=out/'live-extra'/name;work.mkdir(parents=True,exist_ok=True)
 gate(name,['python3','-u',fault/'tests/live/gc_reclamation.py','--mode',mode,'--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--tiny-sustained-seconds','120','--tiny-overload','--replication-pressure','--server-binary',fault/'efsd','--client-binary',fault/'efs-fuse'],timeout=1800,cwd=fault)
for m in ('/data1/efs/mnt','/data1/efs/mnt2'):gate('restore-'+Path(m).name,['/data1/efs/src/scripts/client.sh','127.0.0.1:17432',m])
(out/'amd-split.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
