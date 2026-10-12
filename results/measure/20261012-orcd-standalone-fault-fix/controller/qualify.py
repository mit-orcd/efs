import importlib.util,subprocess,tarfile,io,json,hashlib
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('work/fix-orcd-valgrind');revision='ed749cab212768137a32ee8d3a2b80fa79744215';base='/tmp/efs-repair-qualified-ed749cab-v2';data=io.BytesIO();hashes={}
files=subprocess.check_output(['git','ls-tree','-r','--name-only',revision],cwd=p,text=True).splitlines()
with tarfile.open(fileobj=data,mode='w:gz') as t:
 for name in files:
  if name.startswith(('results/','.git/')):continue
  f=p/name
  if not f.is_file():continue
  content=f.read_bytes()
  if name=='docs/status/README.md':content=subprocess.check_output(['git','show',revision+':'+name],cwd=p)
  info=tarfile.TarInfo(name);info.size=len(content);info.mode=f.stat().st_mode&0o777;info.mtime=int(f.stat().st_mtime);t.addfile(info,io.BytesIO(content));hashes[name]=hashlib.sha256(content).hexdigest()
Path('operations/orcd-fault-fix/qualified-inputs.json').write_text(json.dumps(dict(commit=revision,sha256=hashes),indent=2)+'\n')
print(m.shell('fcstor006',f'test ! -e {base}; mkdir {base}',120),flush=True)
r=subprocess.run(m.ssh_command('fcstor006',f'tar -xzf - -C {base}',m.OPTIONS,m.USER,m.BRIDGE),input=data.getvalue(),capture_output=True,timeout=180);assert r.returncode==0,r.stderr
runtime=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
body=f'''cd {base}
export PATH=/tmp/efs-repair-private-compiler-20261012:$PATH
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={runtime}:{m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2
for mode in fault normal; do
    faults=0; [ "$mode" != fault ] || faults=1
    make clean > /tmp/efs-qualified-v2-$mode-clean.log 2>&1
    make -k -j12 EFS_GIT_ID=ed749cab2127 EFS_FAULTS="$faults" all test > /tmp/efs-qualified-v2-$mode-native.log 2>&1
    printf '0\\n' > /tmp/efs-qualified-v2-$mode-native.exit
    echo QUALIFIED_$mode=0
done
make EFS_GIT_ID=ed749cab2127 tests/faults/fault_sim tests/faults/repair_request > /tmp/efs-qualified-v2-drivers-build.log 2>&1
cc -O1 -g -fsanitize=address,undefined -DEFS_BUILD_ID='"ed749cab2127"' -Iinclude -Isrc/common tests/test_repair_worker.c src/server/repair_worker.c libefs.a -lpthread -lm -ldl -libverbs -o /tmp/efs-qualified-v2-worker-asan
ASAN_OPTIONS=detect_leaks=1 /tmp/efs-qualified-v2-worker-asan > /tmp/efs-qualified-v2-worker-asan.log 2>&1
export EFS_FAULT_BIN={base}
for transport in rdma tcp; do
    bash tests/faults/run_faults.sh /tmp/efs-qualified-v2-$transport-ed749cab --integrate --mode direct --transport "$transport" --rdma-device mlx5_2 --repair-experiment > /tmp/efs-qualified-v2-$transport.log 2>&1
    printf '0\\n' > /tmp/efs-qualified-v2-$transport.exit
    tail -2 /tmp/efs-qualified-v2-$transport.log
done
python3 tests/live/repair_wire.py --binary {base}/efsd --build-id ed749cab2127 --root /tmp/efs-qualified-v2-wire-ed749cab > /tmp/efs-qualified-v2-wire.log 2>&1
sha256sum efsd efs-fuse efs-mgmt tests/faults/repair_request libefs.a > /tmp/efs-qualified-v2-binary-sha256.txt
printf '{revision}\\n' > /tmp/efs-qualified-v2-commit.txt
cat /tmp/efs-qualified-v2-wire.log
'''
print(m.shell('fcstor006',body,14400),flush=True)
