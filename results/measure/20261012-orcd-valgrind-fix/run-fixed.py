import importlib.util,subprocess,tarfile,io,hashlib,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3'];p=Path('operations/orcd-valgrind-fix');repo=Path('work/fix-orcd-valgrind');base=m.BULK+'/qualification-valgrind-fix-8b7391b3f3ed';src='/tmp/efs-vg-fix-8b7391b3f3ed';files=['Makefile','src/common/rdma.c','src/common/rdma_memcheck.h','tests/valgrind_leaks.sh','tests/test_rdma_memcheck.py','tests/tools/build_valgrind_rdma.sh'];buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w') as t:
 for n in files:t.add(repo/n,arcname=n)
(p/'patch-inputs.json').write_text(json.dumps({n:hashlib.sha256((repo/n).read_bytes()).hexdigest() for n in files},indent=2)+'\n')
r=subprocess.run(m.ssh_command('fcstor005','cat > '+base+'/patch.tar',m.OPTIONS,m.USER,m.BRIDGE),input=buf.getvalue(),capture_output=True);assert r.returncode==0,r.stderr
body=f'''test ! -e {src};mkdir {src};tar -xzf {m.BULK}/qualification-release-8b7391b3f3ed/source.tar.gz -C {src};tar -xf {base}/patch.tar -C {src}
cd {src}
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig:${{PKG_CONFIG_PATH:-}}
export LD_LIBRARY_PATH={m.BULK}/deps/libfuse/lib:/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-b239dc7de104/sanitizer-runtime/usr/lib64:${{LD_LIBRARY_PATH:-}}
export PATH=/home/erbmi1/orcd/scratch/efs/orcd-test/qualification-b239dc7de104/sanitizer-runtime/bin:$PATH
export EFS_VG_RDMA_RUNTIME={base} EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 EFS_VG_MODE=direct EFS_VG_PORT=20810 EFS_VG_RDMA_IP=172.16.223.59
set +e
bash tests/valgrind_leaks.sh /tmp/efs-vg-fixed-8b7391b3f3ed > {base}/fixed-full.log 2>&1
rc=$?;echo "$rc" > {base}/fixed-full.exit;tail -15 {base}/fixed-full.log
'''
print(m.shell('fcstor005',body,timeout=14400),flush=True)
