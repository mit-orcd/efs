import os,time,subprocess,shutil
from pathlib import Path
exec(Path('/tmp/amd-all-follow-approved.py').read_text().split('while not (out/')[0])
while not (out/'amd-split.finished').exists():time.sleep(10)
compat=out/'valgrind-compatible-src';compat.mkdir(exist_ok=True)
for n in ('src','include','deps','tests','scripts'):
 shutil.copytree(src/n,compat/n,dirs_exist_ok=True,ignore=shutil.ignore_patterns('*.o','*.a','__pycache__'))
shutil.copy2(src/'Makefile',compat/'Makefile')
flags='-march=x86-64-v3 -mtune=generic -DBLAKE3_NO_AVX512'
env={'EXTRA_DEFS':flags,'EFS_GIT_ID':'28b2089b-release-valgrind-compatible','EFS_GIT_BRANCH':'release-v0.3.0-pre-alpha','EFS_GIT_DESCRIBE':'v0.3.0-pre-alpha-candidate','EFS_VG_MODE':'direct','EFS_VG_PORT':'26320'}
# Fresh tree, no native objects: original assertions and leak criteria retained.
if gate('valgrind-compatible-build',['make','-B','-j8','EXTRA_DEFS='+flags,'EFS_GIT_ID=28b2089b-release-valgrind-compatible','efsd','efs-fuse','efs-mgmt','efs-bench',*[x for line in (compat/'Makefile').read_text().splitlines() if line.startswith('TEST_BINS =') for x in line.split('=',1)[1].split()]],env,timeout=1800,cwd=compat)==0:
 gate('valgrind-direct-compatible',['bash','tests/valgrind_leaks.sh','/tmp/efs-all-28b2089b-vg-direct-compatible'],env,timeout=3600,cwd=compat)
(out/'valgrind-compatible.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())+'\n')
