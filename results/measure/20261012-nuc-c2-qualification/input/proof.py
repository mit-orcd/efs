import json,hashlib,os,time,sys
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');installed=Path('/data1/efs/src')
def digest(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
images=json.loads((base/'clock-install/images.json').read_text())
for n,r in images.items():assert digest(installed/n)==r['sha256'],n
rows=json.loads((base/'clock-cold/after.json').read_text())['payloads'];assert len(rows)==36
for r in rows:assert digest(r['path'])==r['sha256'],r['path']
mapped=[]
for p in Path('/proc').iterdir():
 if not p.name.isdigit():continue
 try:
  target=os.readlink(p/'exe');kind=Path(target).name
  if target not in (str(installed/'efsd'),str(installed/'efs-fuse')):continue
  h=digest(p/'exe');assert h==images[kind]['sha256']
  env={x.split('=',1)[0]:x.split('=',1)[1] for x in (p/'environ').read_bytes().decode().split('\0') if x.startswith('EFS_') and '=' in x}
  mapped.append(dict(pid=int(p.name),kind=kind,sha256=h,args=(p/'cmdline').read_bytes().decode().split('\0'),configuration=env))
 except (FileNotFoundError,ProcessLookupError,PermissionError):continue
assert sum(p['kind']=='efsd' for p in mapped)==4 and sum(p['kind']=='efs-fuse' for p in mapped)==2
assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2')) and Path('/data1/efs/.mkfs-done').is_file()
Path(sys.argv[1]).write_text(json.dumps(dict(utc=time.strftime('%FT%TZ',time.gmtime()),images=images,processes=mapped,payloads=rows),indent=2))
print('preserved f52 4 daemons +2 clients +36 payloads PASS')
