import importlib.util,subprocess,tarfile,io,hashlib,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('work/fix-orcd-valgrind/results/measure/20261012-orcd-standalone-fault-fix');p.mkdir(parents=True,exist_ok=True)
script=r'''import pathlib,tarfile,sys
roots=['efs-fault-fix-rdma-results-20261012','efs-fault-repair-worker-results-20261012','efs-fault-repair-worker-results-v2-20261012','efs-fault-repair-worker-results-v3-20261012','efs-repair-final-rdma-20261012','efs-repair-final-tcp-20261012','efs-repair-diag-tcp-1-20261012','efs-repair-diag2-tcp-1-20261012','efs-repair-pool-negative-results-20261012','efs-repair-pool-negative-repeat2-20261012','efs-qualified-v3-rdma-ed749cab','efs-qualified-v3-tcp-ed749cab','efs-qualified-v3-wire-ed749cab']
roots += ['efs-repair-fixed-tcp-%d-20261012'%i for i in (1,2,3)]
files=set()
for root in roots:
 p=pathlib.Path('/tmp')/root
 if p.exists():
  files.update(x for x in p.rglob('*') if x.is_file() and (x.suffix in ('.log','.json','.jsonl') or x.name.endswith('-pid')) and not any(n in ('data','meta','mdraft') for n in x.relative_to(p).parts))
for pattern in ('efs-qualified*.log','efs-qualified*.exit','efs-qualified*.txt','efs-repair-*.log'):
 files.update(pathlib.Path('/tmp').glob(pattern))
with tarfile.open(fileobj=sys.stdout.buffer,mode='w|gz') as t:
 for f in sorted(files):t.add(f,arcname=str(f.relative_to('/tmp')),recursive=False)
'''
import shlex
r=subprocess.run(m.ssh_command('fcstor006','python3 -c '+shlex.quote(script),m.OPTIONS,m.USER,m.BRIDGE),capture_output=True,timeout=300);assert r.returncode==0,r.stderr
with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:gz') as t:
 assert all(x.isfile() and not Path(x.name).is_absolute() and '..' not in Path(x.name).parts for x in t.getmembers());t.extractall(p/'remote')
for f in Path('operations/orcd-fault-fix').glob('*'):
 if f.is_file() and f.suffix in ('.py','.json','.log'):(p/'controller').mkdir(exist_ok=True);(p/'controller'/f.name).write_bytes(f.read_bytes())
files={str(f.relative_to(p)):hashlib.sha256(f.read_bytes()).hexdigest() for f in p.rglob('*') if f.is_file() and f.name!='artifact-sha256.json'}
(p/'artifact-sha256.json').write_text(json.dumps(files,indent=2)+'\n');print(str(len(files))+' evidence files retained and hashed')
