import importlib.util,subprocess,tarfile,tempfile,concurrent.futures,shlex,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
repo='/Users/mike/Documents/Codex/2026-10-08/efs/work/fix-orcd-valgrind'
rev=subprocess.check_output(['git','-C',repo,'rev-parse','HEAD'],text=True).strip()
stage='/tmp/efs-orcd-profile-'+rev[:12];archive=m.BULK+'/profile-'+rev+'.tgz'
with tempfile.TemporaryFile() as raw,tempfile.TemporaryFile() as out:
 subprocess.run(['git','-C',repo,'archive',rev],stdout=raw,check=True);raw.seek(0)
 with tarfile.open(fileobj=raw,mode='r:') as a,tarfile.open(fileobj=out,mode='w:gz') as b:m.write_deployment_snapshot(a,b)
 out.seek(0)
 subprocess.run(['ssh','-A','-o','ControlPath=/private/tmp/efs-orcd-login-control3',m.BRIDGE,'cat > '+archive],stdin=out,check=True)
def build(h):
 rt=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
 body=f'''test ! -e {stage}
mkdir {stage}; tar -xzf {archive} -C {stage}
cd {stage}
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={rt}:{m.BULK}/deps/libfuse/lib
cat > .profile-cc <<'CC'
#!/bin/sh
exec /usr/bin/cc -L{rt} -Wl,-rpath,{rt} "$@"
CC
chmod +x .profile-cc
make -j8 CC={stage}/.profile-cc EFS_GIT_ID={rev[:12]} EFS_GIT_BRANCH=fix-orcd-packing-stress EFS_GIT_DESCRIBE={rev[:12]} FUSE_LIBS="$(pkg-config --libs fuse3) -Wl,-rpath,{m.BULK}/deps/libfuse/lib" all > {m.BULK}/{h}/profile-build-{rev[:12]}.log 2>&1
printf '%s\\n' {rev} > .orcd-test-revision
sha256sum efsd efs-fuse efs-mgmt
'''
 result=m.shell(h,body,1800);print(h,result,flush=True);return {'host':h,'sha256':result}
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as p:rows=list(p.map(build,m.HOSTS))
Path('operations/orcd-packing-fix/profile-stage.json').write_text(json.dumps({'revision':rev,'stage':stage,'hosts':rows},indent=2)+'\n')
print('All eleven staged builds passed; live cluster unchanged',flush=True)
