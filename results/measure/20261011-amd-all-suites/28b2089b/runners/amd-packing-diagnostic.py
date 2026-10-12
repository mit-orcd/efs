from pathlib import Path
import time
exec(Path('/tmp/amd-all-follow-approved.py').read_text().split('while not (out/')[0])
name='packing-seven-direct-profiled';work=out/'live-diagnostic'/name;work.mkdir(parents=True,exist_ok=True)
try:
 gate(name,['python3','-u','tests/live/gc_reclamation.py','--mode','direct','--transport','tcp','--root',work,'--root',work,'--port','24210','--mount','/data1/efs/mnt','--peer-mount','/data1/efs/mnt2','--mounted-packing-only','--packing-clients','7','--posix-jobs','16','--posix','--continue-posix-failure','--fixture-quota','4G','--server-binary',out/'normal-binaries/efsd','--client-binary',out/'normal-binaries/efs-fuse'],{'EFS_RPC_PROF':'1','EFS_READ_PROF':'1','EFS_RAFT_OBS':'1','EFS_BATCH_OBS':'1','EFS_POSIX_PHASE_TRACE':'1','EFS_QUERY_WORK_TRACE':'1','EFS_QUERY_STAGE_TRACE':'1'},timeout=1800)
finally:
 for m in ('/data1/efs/mnt','/data1/efs/mnt2'):gate('restore-diagnostic-'+Path(m).name,['/data1/efs/src/scripts/client.sh','127.0.0.1:17432',m])
(out/'packing-diagnostic.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
