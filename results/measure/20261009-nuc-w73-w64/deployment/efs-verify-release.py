from pathlib import Path
import json,subprocess,hashlib
base=Path('/data1/efs');rows=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  exe=str((p/'exe').resolve());args=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode();env=(p/'environ').read_bytes().split(b'\0')
 except (OSError,PermissionError):continue
 if exe in (str(base/'src/efsd'),str(base/'src/efs-fuse')):
  selected={x.split(b'=',1)[0].decode():x.split(b'=',1)[1].decode() for x in env if x.startswith((b'EFS_TRANSPORT=',b'EFS_COMMANDS=',b'EFS_COMMANDS_SERVER='))}
  assert selected.get('EFS_TRANSPORT')=='tcp',(args,selected)
  assert selected.get('EFS_COMMANDS')=='0' and selected.get('EFS_COMMANDS_SERVER')=='0',(args,selected)
  if exe.endswith('/efsd'):assert '--direct-io' in args
  rows.append({'pid':int(p.name),'exe':exe,'args':args,'environment':selected})
assert sum(x['exe'].endswith('/efsd') for x in rows)==4,rows
assert sum(x['exe'].endswith('/efs-fuse') for x in rows)==2,rows
mounts=[]
for m in ('mnt','mnt2'):
 mounts.append(subprocess.check_output(['findmnt','-rn','-M',str(base/m),'-o','TARGET,FSTYPE,OPTIONS'],text=True).strip())
versions={n:subprocess.check_output([str(base/'src'/n),'--version'],text=True).strip() for n in ('efsd','efs-fuse','efs-mgmt','efs-bench','efs-query')}
for n,v in versions.items():assert '393f5bae2c00' in v,(n,v)
src=base/'src';d=hashlib.sha256();paths=[src/'Makefile']+[p for f in ('src','include') for p in (src/f).rglob('*') if p.suffix in ('.c','.h')]
for p in sorted(paths):d.update(str(p.relative_to(src)).encode()+b'\0'+p.read_bytes())
assert d.hexdigest()=='7d080221cba7aec4163a3a2a1732d5a69e160dd7d10f8499e110dc6766150fc2'
print(json.dumps({'processes':rows,'mounts':mounts,'versions':versions,'source_tree_sha256':d.hexdigest(),'binary_sha256':{n:hashlib.sha256((src/n).read_bytes()).hexdigest() for n in versions}},indent=2))
