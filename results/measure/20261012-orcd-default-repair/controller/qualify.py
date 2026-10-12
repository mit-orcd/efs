import importlib.util, subprocess, tarfile, io, hashlib, json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('work/fix-orcd-valgrind');revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=p,text=True).strip();short=revision[:12]
base='/tmp/efs-repair-default-'+short;data=io.BytesIO();hashes={}
files=subprocess.check_output(['git','ls-tree','-r','--name-only',revision],cwd=p,text=True).splitlines()
with tarfile.open(fileobj=data,mode='w:gz') as t:
 for name in files:
  if name.startswith('results/') and not name.startswith('results/measure/20261012-orcd-standalone-fault-fix/'):continue
  f=p/name
  if not f.is_file():continue
  content=f.read_bytes();info=tarfile.TarInfo(name);info.size=len(content);info.mode=f.stat().st_mode&0o777;info.mtime=int(f.stat().st_mtime);t.addfile(info,io.BytesIO(content));hashes[name]=hashlib.sha256(content).hexdigest()
Path('operations/orcd-repair-default/inputs.json').write_text(json.dumps(dict(commit=revision,base=base,sha256=hashes),indent=2)+'\n')
print(m.shell('fcstor006',f'test ! -e {base}; cp -a /tmp/efs-repair-qualified-ed749cab-v2 {base}',180),flush=True)
r=subprocess.run(m.ssh_command('fcstor006',f'tar -xzf - -C {base}',m.OPTIONS,m.USER,m.BRIDGE),input=data.getvalue(),capture_output=True,timeout=180);assert r.returncode==0,r.stderr
runtime=m.BULK+'/qualification-b239dc7de104/sanitizer-runtime/usr/lib64'
body=f'''cd {base}
export PATH=/tmp/efs-repair-private-compiler-20261012:$PATH
export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig
export LD_LIBRARY_PATH={runtime}:{m.BULK}/deps/libfuse/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_2
python3 docs/check-architecture.py > /tmp/efs-repair-default-doc.log 2>&1
for mode in fault normal; do
 faults=0; [ "$mode" != fault ] || faults=1
 make clean > /tmp/efs-repair-default-$mode-clean.log 2>&1
 make -k -j12 EFS_GIT_ID={short} EFS_FAULTS="$faults" all test > /tmp/efs-repair-default-$mode-native.log 2>&1
 printf '0\\n' > /tmp/efs-repair-default-$mode-native.exit
 echo NATIVE_$mode=0
done
make EFS_GIT_ID={short} tests/faults/fault_sim tests/faults/repair_request > /tmp/efs-repair-default-drivers.log 2>&1
export EFS_FAULT_BIN={base}
unset EFS_REPAIR_EXPERIMENT EFS_REPAIR_INIT_STORE EFS_PACKED_WRITES
for transport in rdma tcp; do
 bash tests/faults/run_faults.sh /tmp/efs-repair-default-$transport-{short} --integrate --mode direct --transport "$transport" --rdma-device mlx5_2 > /tmp/efs-repair-default-$transport.log 2>&1
 printf '0\\n' > /tmp/efs-repair-default-$transport.exit
 tail -2 /tmp/efs-repair-default-$transport.log
done
bash tests/faults/run_faults.sh /tmp/efs-repair-default-buffered-{short} --integrate > /tmp/efs-repair-default-buffered.log 2>&1
printf '0\\n' > /tmp/efs-repair-default-buffered.exit
tail -2 /tmp/efs-repair-default-buffered.log
python3 tests/live/repair_wire.py --binary {base}/efsd --build-id {short} --root /tmp/efs-repair-default-wire-{short} > /tmp/efs-repair-default-wire.log 2>&1
sha256sum efsd efs-fuse efs-mgmt tests/faults/fault_sim tests/faults/repair_request > /tmp/efs-repair-default-binaries.txt
cat /tmp/efs-repair-default-wire.log
'''
print(m.shell('fcstor006',body,14400),flush=True)
