import importlib.util,concurrent.futures,json,shlex
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
def one(h):
 code='''import pathlib,os,json,hashlib,subprocess
rows=[]
for p in pathlib.Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  exe=os.readlink(p/'exe')
  if pathlib.Path(exe).name not in ('efsd','efs-fuse'):continue
  args=(p/'cmdline').read_bytes().replace(bytes([0]),b' ').decode();uid=p.stat().st_uid
  assert uid==246111 and exe in ('/tmp/efs/efsd','/tmp/efs/efs-fuse'),(exe,uid,args)
  rows.append(dict(pid=int(p.name),exe=exe,uid=uid,args=args,sha256=hashlib.sha256(pathlib.Path(exe).read_bytes()).hexdigest()))
 except (FileNotFoundError,PermissionError,ProcessLookupError):pass
assert len(rows)==1,rows
rev=pathlib.Path('/tmp/efs/.orcd-test-revision').read_text().strip();assert rev=='4fe291b0218bc37cd964c925ecebcea4df93e99f',rev
p=subprocess.run(['findmnt','-rn','-M','/tmp/efs-c2bf-mount'],capture_output=True,text=True);assert p.returncode!=0,p.stdout
print(json.dumps(dict(original_revision=rev,processes=rows,private_mount_absent=True)))'''
 return dict(host=h,**json.loads(m.shell(h,'python3 -c '+shlex.quote(code),timeout=90)))
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:rows=list(pool.map(one,m.HOSTS))
p=Path('work/devel-orcd/results/measure/20261011-orcd-coalescing-rollout/c2bfb9e9a281-private/final-original-services.json');p.write_text(json.dumps(rows,indent=2)+'\n');print('all11 original exactrevision/nonroot/executable; c2 private mounts/processes absent PASS')
