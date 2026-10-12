import importlib.util,subprocess,tarfile,io,json,hashlib
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3'];p=Path('operations/orcd-valgrind-fix');base=m.BULK+'/qualification-valgrind-fix-8b7391b3f3ed';buf=io.BytesIO()
with tarfile.open(fileobj=buf,mode='w') as t:
 for name in ['rdma-core-v59.0.tar.gz','valgrind-3.26.0.tar.bz2']:t.add(p/name,arcname=name)
r=subprocess.run(m.ssh_command('fcstor005','mkdir -p '+base+' && tar -xf - -C '+base,m.OPTIONS,m.USER,m.BRIDGE),input=buf.getvalue(),capture_output=True);assert r.returncode==0,r.stderr
body=f'''cd {base}
test ! -e rdma-core-59.0;tar -xzf rdma-core-v59.0.tar.gz
tar -xjf valgrind-3.26.0.tar.bz2 valgrind-3.26.0/include/valgrind.h valgrind-3.26.0/memcheck/memcheck.h valgrind-3.26.0/drd/drd.h
mkdir -p vg-headers/valgrind
cp valgrind-3.26.0/include/valgrind.h valgrind-3.26.0/memcheck/memcheck.h valgrind-3.26.0/drd/drd.h vg-headers/valgrind/
cmake -S rdma-core-59.0 -B rdma-core-build -DIN_PLACE=1 -DENABLE_VALGRIND=ON -DNO_MAN_PAGES=ON -DENABLE_RESOLVE_NEIGH=OFF -DENABLE_STATIC=OFF -DCMAKE_C_FLAGS="-I{base}/vg-headers" > configure.log 2>&1
# Do not accept the upstream fallback dummy Valgrind headers.
grep -q '^HAVE_VALGRIND_MEMCHECK:INTERNAL=1$' rdma-core-build/CMakeCache.txt
grep -q '^HAVE_VALGRIND_DRD:INTERNAL=1$' rdma-core-build/CMakeCache.txt
cmake --build rdma-core-build --target ibverbs mlx5 -j4 > build.log 2>&1
set +e
cd /tmp/efs-orcd-vg-8b7391b3f3ed
LD_LIBRARY_PATH={base}/rdma-core-build/lib EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2 timeout 180 valgrind --track-origins=yes --error-exitcode=99 --errors-for-leak-kinds=definite,indirect --leak-check=full --show-leak-kinds=all --log-file={base}/annotated-rdma.vg ./tests/test_rdma_xprt > {base}/annotated-rdma.out 2>&1
rc=$?;echo "$rc" > {base}/annotated-rdma.exit;cat {base}/annotated-rdma.out;tail -18 {base}/annotated-rdma.vg
'''
print(m.shell('fcstor005',body,timeout=3600),flush=True)
