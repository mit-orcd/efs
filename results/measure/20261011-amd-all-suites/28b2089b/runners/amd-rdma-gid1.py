from pathlib import Path
import time
exec(Path('/tmp/amd-all-follow-approved.py').read_text().split('while not (out/')[0])
while not (out/'valgrind-compatible.finished').exists():time.sleep(10)
root=Path('/tmp/efs-rdma-28b2089b-gid1');root.mkdir(exist_ok=True)
gate('rdma-posix-gid1',['bash','tests/live/rdma_posix_gates.sh'],{'EFS_TRANSPORT':'rdma','EFS_RDMA_DEV':'rxe0','EFS_RDMA_GID_INDEX':'1','EFS_GATE_ROOT':str(root)},timeout=1800)
(out/'rdma-gid1.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
