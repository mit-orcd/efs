import subprocess,tarfile,io,json,hashlib,shutil,shlex
from pathlib import Path
p=Path('operations/orcd-v031-rdma');meta=json.loads((p/'profile-stage.json').read_text());tag=meta['revision'][:12]
dest=Path('work/fix-orcd-valgrind/results/orcd-v031-rdma-direct-20261012-'+tag[:8]);dest.mkdir(parents=True,exist_ok=True)
for f in p.iterdir():
 if f.is_file() and f.suffix in ('.py','.json','.log'):shutil.copy2(f,dest/f.name)
bulk='/home/erbmi1/orcd/scratch/efs/orcd-test';names=[f'fcstor006/profile-native-{tag}.log',f'fcstor006/v031-{tag}-prerequisites.json']
prereq=subprocess.check_output(['ssh','-A','-o','ControlPath=/private/tmp/efs-orcd-login-control3','erbmi1@orcd-login.mit.edu','cat '+bulk+'/'+names[-1]],text=True)
for row in json.loads(prereq):names.append(f'fcstor006/v031-{tag}-'+row['name']+'.log')
for h in [f'fcstor{i:03}' for i in range(7,14)]:
 for t in [tag,tag+'-repeat1']:
  for ext in ['tsv','log','exit']:names.append(f'{h}/profile-posix-{t}.{ext}')
for t in [tag,tag+'-repeat1']:
 for ext in ['tsv','log','exit']:names.append(f'fcstor007/profile-peer-{t}.{ext}')
cmd='tar -czf - -C '+bulk+' '+shlex.join(names)
r=subprocess.run(['ssh','-A','-o','ControlPath=/private/tmp/efs-orcd-login-control3','erbmi1@orcd-login.mit.edu',cmd],capture_output=True,timeout=120);r.check_returncode()
with tarfile.open(fileobj=io.BytesIO(r.stdout),mode='r:gz') as t:
 for member in t:
  assert member.isfile() and member.name in names
  out=dest/'remote'/member.name;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(t.extractfile(member).read())
manifest={str(f.relative_to(dest)):hashlib.sha256(f.read_bytes()).hexdigest() for f in dest.rglob('*') if f.is_file() and f.name!='artifact-sha256.json'}
(dest/'artifact-sha256.json').write_text(json.dumps(manifest,indent=2)+'\n');print('collected',len(manifest),'files',flush=True)
