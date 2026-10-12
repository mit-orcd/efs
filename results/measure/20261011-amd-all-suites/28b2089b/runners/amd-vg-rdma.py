from pathlib import Path
import time
exec(Path('/tmp/amd-all-follow-approved.py').read_text().split('while not (out/')[0])
compat=out/'valgrind-compatible-src'
gate('valgrind-rdma-compatible',['bash','tests/valgrind_leaks.sh','/tmp/efs-all-28b2089b-vg-rdma-compatible'],{'EXTRA_DEFS':'-march=x86-64-v3 -mtune=generic -DBLAKE3_NO_AVX512','EFS_GIT_ID':'28b2089b-release-valgrind-compatible','EFS_GIT_BRANCH':'release-v0.3.0-pre-alpha','EFS_GIT_DESCRIBE':'v0.3.0-pre-alpha-candidate','EFS_TRANSPORT':'auto','EFS_RDMA_DEV':'rxe0','EFS_RDMA_GID_INDEX':'1','EFS_VG_MODE':'direct','EFS_VG_PORT':'26320','EFS_VG_RDMA_IP':'10.0.0.206','EFS_VG_SKIP_UNITS':'1'},timeout=3600,cwd=compat)
(out/'valgrind-rdma.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
