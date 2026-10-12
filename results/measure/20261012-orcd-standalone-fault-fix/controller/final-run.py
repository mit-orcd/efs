import importlib.util,subprocess,tarfile,io,json,hashlib
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('work/fix-orcd-valgrind');paths=subprocess.check_output(['git','diff','--name-only','8b7391b3'],cwd=p,text=True).splitlines()+['src/server/repair_worker.c','tests/faults/repair_request.c','tests/test_repair_worker.c'];paths=sorted(set(x for x in paths if not x.startswith(('results/','docs/')) and (p/x).is_file()))
b=io.BytesIO()
with tarfile.open(fileobj=b,mode='w:gz') as t:
 for name in paths:t.add(p/name,arcname=name)
root='/tmp/efs-fault-repair-worker-20261012';r=subprocess.run(m.ssh_command('fcstor006',f'tar -xzf - -C {root}',m.OPTIONS,m.USER,m.BRIDGE),input=b.getvalue(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
Path('operations/orcd-fault-fix/final-inputs.json').write_text(json.dumps({n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in paths},indent=2)+'\n')
runtime=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
body=f'''cd {root}
mkdir -p /tmp/efs-repair-private-compiler-20261012
cat > /tmp/efs-repair-private-compiler-20261012/cc <<'EOF'
#!/bin/sh
exec /usr/bin/cc -L{runtime} -Wl,-rpath,{runtime} "$@"
EOF
chmod +x /tmp/efs-repair-private-compiler-20261012/cc
export PATH=/tmp/efs-repair-private-compiler-20261012:$PATH
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={runtime}:{m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
make -k -j12 EFS_GIT_ID=orcd-repair-final-wip EFS_FAULTS=1 all test > /tmp/efs-repair-final-native.log 2>&1
printf '0\\n' > /tmp/efs-repair-final-native.exit
make EFS_GIT_ID=orcd-repair-final-wip EFS_FAULTS=1 tests/faults/fault_sim tests/faults/repair_request >/tmp/efs-repair-final-drivers-build.log 2>&1
cc -O1 -g -fsanitize=address,undefined -DEFS_BUILD_ID='"orcd-repair-final-wip"' -Iinclude -Isrc/common tests/test_repair_worker.c src/server/repair_worker.c libefs.a -lpthread -lm -ldl -libverbs -o /tmp/efs-repair-worker-asan
ASAN_OPTIONS=detect_leaks=1 /tmp/efs-repair-worker-asan > /tmp/efs-repair-worker-asan.log 2>&1
export EFS_FAULT_BIN={root}
for transport in rdma tcp; do
    bash tests/faults/run_faults.sh /tmp/efs-repair-final-$transport-20261012 --integrate --mode direct --transport "$transport" --rdma-device mlx5_2 --repair-experiment > /tmp/efs-repair-final-$transport.log 2>&1
    printf '0\\n' > /tmp/efs-repair-final-$transport.exit
    tail -2 /tmp/efs-repair-final-$transport.log
done
cat /tmp/efs-repair-worker-asan.log
'''
print(m.shell('fcstor006',body,7200),flush=True)
