from pathlib import Path
import subprocess,json,os,time,socket
source=Path('/data1/efs/amd-v031-buffered-15e2a531-20261012/source')
while not (source.parent/'finished').exists():time.sleep(10)
assert all(r['exit']==0 for r in json.loads((source.parent/'results.json').read_text())), 'native prerequisite failed'

out=Path('/data1/efs/amd-v031-buffered-15e2a531-20261012-overwrite-retry1');out.mkdir(exist_ok=True)
for port in range(25320,25324):
 s=socket.socket();assert s.connect_ex(('127.0.0.1',port))!=0,port;s.close()
mount=Path('/tmp/efs-amd-buffered-retry1-isolated-15e2a5310df1dd0fd4e307a9508c95d513d14672-mnt');peer=Path('/tmp/efs-amd-buffered-retry1-isolated-15e2a5310df1dd0fd4e307a9508c95d513d14672-peer')
for p in (mount,peer):
 p.mkdir(exist_ok=True);assert not os.path.ismount(p) and not any(p.iterdir()),p
cmd=['python3','-u','tests/live/gc_reclamation.py','--mode','buffered','--transport','tcp','--root',str(out),'--root',str(out),'--port','25320','--mount',str(mount),'--peer-mount',str(peer),'--disable-efs-commands','--ordinary-overwrite-only','--fixture-quota','4G','--server-binary',str(source/'efsd'),'--client-binary',str(source/'efs-fuse')]
extra={'EFS_REPAIR_EXPERIMENT':'0','EFS_REPAIR_INIT_STORE':'0','EFS_PACKED_WRITES':'0','EFS_RAFT_FLOW':'0','EFS_RAFT_FLOW_EARLY_RETIRE':'0','EFS_RAFT_GROUP_DRAIN':'0'}
clean_env=dict(os.environ)
for key in ('EFS_RPC_PROF','EFS_READ_PROF','EFS_RAFT_OBS','EFS_BATCH_OBS','EFS_POSIX_PHASE_TRACE','EFS_QUERY_STAGE_TRACE','EFS_QUERY_WORK_SOCKET','EFS_QUERY_WORK_TRACE'):
 clean_env.pop(key,None)
(out/'command.json').write_text(json.dumps({'source':'15e2a5310df1dd0fd4e307a9508c95d513d14672','cwd':str(source),'command':cmd,'environment':extra,'scope':'ordinary overwrite, cold restart and physical deletion; .efs disabled; not full broad acceptance'},indent=2))
start=time.time()
with (out/'run.log').open('w') as log:
 p=subprocess.Popen(cmd,cwd=source,env=dict(clean_env,**extra),stdout=log,stderr=subprocess.STDOUT)
 (out/'pid').write_text(str(p.pid))
 rc=p.wait()
(out/'run.exit').write_text(str(rc)+'\n')
(out/'result.json').write_text(json.dumps({'source':'15e2a5310df1dd0fd4e307a9508c95d513d14672','exit':rc,'status':'PASS' if rc==0 else 'FAIL','duration_seconds':time.time()-start,'acceptance':False},indent=2))
(out/'finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
