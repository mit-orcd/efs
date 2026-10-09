from pathlib import Path
import os,json,subprocess,hashlib,time
base=Path('/data1/efs');rows=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  exe=str((p/'exe').resolve());args=(p/'cmdline').read_bytes().replace(b'\0',b' ').decode();env=(p/'environ').read_bytes().split(b'\0')
 except (OSError,PermissionError):continue
 if exe not in (str(base/'src/efsd'),str(base/'src/efs-fuse')):continue
 selected={x.split(b'=',1)[0].decode():x.split(b'=',1)[1].decode() for x in env if x.startswith((b'EFS_TRANSPORT=',b'EFS_RDMA_DEV=',b'EFS_RDMA_GID_INDEX=',b'EFS_COMMANDS=',b'EFS_COMMANDS_SERVER='))}
 assert selected.get('EFS_TRANSPORT')=='rdma',(args,selected)
 assert selected.get('EFS_RDMA_DEV')=='rxe0' and selected.get('EFS_RDMA_GID_INDEX')=='1',selected
 assert selected.get('EFS_COMMANDS')=='0' and selected.get('EFS_COMMANDS_SERVER')=='0',selected
 if exe.endswith('/efsd'):assert '--direct-io' in args
 devices=[]
 for fd in (p/'fd').iterdir():
  try:target=os.readlink(fd)
  except FileNotFoundError:continue
  if '/dev/infiniband/uverbs' in target:devices.append(target)
 assert devices,(args,'no verbs fd')
 rows.append({'pid':int(p.name),'exe':exe,'args':args,'environment':selected,'verbs_devices':sorted(set(devices))})
assert sum(x['exe'].endswith('/efsd') for x in rows)==4,rows
assert sum(x['exe'].endswith('/efs-fuse') for x in rows)==2,rows
mounts=[subprocess.check_output(['findmnt','-rn','-M',str(base/m),'-o','TARGET,FSTYPE,OPTIONS'],text=True).strip() for m in ('mnt','mnt2')]
versions={n:subprocess.check_output([str(base/'src'/n),'--version'],text=True).strip() for n in ('efsd','efs-fuse','efs-mgmt','efs-bench','efs-query')}
assert all('f6233987b9d1' in v for v in versions.values()),versions
src=base/'src';d=hashlib.sha256();paths=[src/'Makefile']+[p for f in ('src','include') for p in (src/f).rglob('*') if p.suffix in ('.c','.h')]
for p in sorted(paths):d.update(str(p.relative_to(src)).encode()+b'\0'+p.read_bytes())
print(json.dumps({'time_unix':time.time(),'processes':rows,'mounts':mounts,'versions':versions,'source_tree_sha256':d.hexdigest(),'binary_sha256':{n:hashlib.sha256((src/n).read_bytes()).hexdigest() for n in versions}},indent=2))
