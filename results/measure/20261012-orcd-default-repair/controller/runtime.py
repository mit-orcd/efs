import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
rev='8b7391b3f3edcf9fc99440d9aad3c240325acb83'
def one(h):
 code=f'''import os,pathlib,json,hashlib,subprocess
rows=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  name=(p/'comm').read_text().strip()
  if name not in ('efsd','efs-fuse'):continue
  if p.stat().st_uid!=os.getuid():continue
  exe=os.readlink(p/'exe');assert exe=='/tmp/efs/'+name,exe
  args=(p/'cmdline').read_bytes().replace(bytes([0]),b' ').decode();env=dict(x.split('=',1) for x in (p/'environ').read_bytes().decode().split('\\0') if '=' in x)
  devices=[os.readlink(x) for x in (p/'fd').iterdir() if x.exists() and 'uverbs' in os.readlink(x)]
  assert devices,('RDMA device FD absent',name,args)
  assert env.get('EFS_TRANSPORT')=='rdma' and env.get('EFS_RDMA_DEV')=='mlx5_2',env
  assert p.stat().st_uid==246111
  if name=='efsd':assert '--direct-io' in args and '--no-direct-io' not in args,args
  else:assert env.get('EFS_PACKED_WRITES')=='1',env
  sha=hashlib.sha256(pathlib.Path(exe).read_bytes()).hexdigest()
  rows.append(dict(pid=int(p.name),comm=name,uid=p.stat().st_uid,args=args,exe=exe,exe_sha256=sha,rdma_device_fds=devices,environment={{k:env.get(k) for k in ['EFS_TRANSPORT','EFS_RDMA_DEV','EFS_PACKED_WRITES','EFS_QUERY_DISK_MAX_MIB','EFS_QUERY_DISK_TOTAL_MIB','EFS_RAFT_FLOW','EFS_RAFT_FLOW_EARLY_RETIRE','EFS_RAFT_GROUP_DRAIN','EFS_ADMISSION_PRESSURE','EFS_REPAIR_EXPERIMENT','EFS_REPAIR_INIT_STORE']}}))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
assert len(rows)==1,rows
source=pathlib.Path('/tmp/efs/.orcd-test-revision').read_text().strip();assert source=={rev!r},source
mount=subprocess.run(['findmnt','-rn','-M','/tmp/efs-mount'],capture_output=True,text=True)
print(json.dumps(dict(host={h!r},source=source,processes=rows,mount=mount.stdout.strip())))
'''
 return json.loads(m.shell(h,'python3 -c '+shlex.quote(code),timeout=60))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.HOSTS))
for r in rows:
 if r['host'] in m.CLIENTS:assert 'fuse' in r['mount'],r
p=Path('operations/orcd-repair-default');(p/'runtime.json').write_text(json.dumps(rows,indent=2)+'\n');print('all11 exactrelease/nonroot/native RDMA uverbs;4directIO/7pack1mounts PASS',flush=True)
