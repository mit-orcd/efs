import importlib.util,subprocess,tarfile,io,json,hashlib
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('work/fix-orcd-valgrind');paths=subprocess.check_output(['git','diff','--name-only','8b7391b3'],cwd=p,text=True).splitlines()+['src/server/repair_worker.c'];paths=sorted(set(x for x in paths if not x.startswith(('results/','docs/')) and (p/x).is_file()))
b=io.BytesIO()
with tarfile.open(fileobj=b,mode='w:gz') as t:
 for name in paths:t.add(p/name,arcname=name)
root='/tmp/efs-fault-repair-worker-20261012'
print(m.shell('fcstor006',f'test ! -e {root}; cp -a /tmp/efs-orcd-tests-8b7391b3f3ed {root}',120),flush=True)
r=subprocess.run(m.ssh_command('fcstor006',f'tar -xzf - -C {root}',m.OPTIONS,m.USER,m.BRIDGE),input=b.getvalue(),capture_output=True,timeout=120);assert r.returncode==0,r.stderr
Path('operations/orcd-fault-fix/worker-inputs.json').write_text(json.dumps({n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in paths},indent=2))
print(m.shell('fcstor006',f'cd {root}; export PKG_CONFIG_PATH={m.BULK}/deps/libfuse/lib/pkgconfig; make clean >/dev/null; make -j12 EFS_GIT_ID=orcd-repair-worker-wip EFS_FAULTS=1 efsd efs-fuse efs-mgmt libefs.a tests/faults/fault_sim > /tmp/efs-fault-repair-worker-build.log 2>&1; tail -5 /tmp/efs-fault-repair-worker-build.log',1200),flush=True)
