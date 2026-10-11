#!/usr/bin/env python3
"""Read-only final restored runtime and prior preserved-payload checks."""
import hashlib,json,os,socket,subprocess,time
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');deadline=time.monotonic()+7200
while True:
 loaded=base/'loaded-append-v2'
 if (loaded/'restore-exit.txt').exists():break
 if (loaded/'failure.txt').exists() and not (loaded/'mnt-stop-exit.txt').exists():
  assert (base/'block-traces/restore-exit.txt').exists();break
 assert time.monotonic()<deadline,'final restoration wait timed out'
 time.sleep(.5)
prior=json.loads((base/'prior-final-service.json').read_text());installed=Path('/data1/efs/src')
images={}
for name,old in prior['installed_images'].items():
 path=installed/name
 images[name]=dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),version=subprocess.check_output([str(path),'--version'],text=True))
 assert images[name]==old,('installed image changed',name)
processes=[]
for proc in Path('/proc').iterdir():
 if not proc.name.isdigit():continue
 try:
  cmd=proc.joinpath('cmdline').read_bytes().split(b'\0')
  if cmd and cmd[0].decode() in (str(installed/'efsd'),str(installed/'efs-fuse')):
   kind=Path(cmd[0].decode()).name
   digest=hashlib.sha256(proc.joinpath('exe').read_bytes()).hexdigest()
   assert digest==images[kind]['sha256']
   processes.append(dict(pid=int(proc.name),kind=kind,cmdline=[v.decode() for v in cmd if v],sha256=digest))
 except (FileNotFoundError,ProcessLookupError):continue
assert sum(r['kind']=='efsd' for r in processes)==4
assert sum(r['kind']=='efs-fuse' for r in processes)==2
for port in range(17432,17436):
 with socket.create_connection(('127.0.0.1',port),3):pass
assert all(os.path.ismount('/data1/efs/'+m) for m in ('mnt','mnt2'))
samples=prior['existing_payload_samples_equal_on_both_mounts']
for sample in samples:
 for mount in ('mnt','mnt2'):
  path=Path('/data1/efs',mount)/sample['path']
  assert path.stat().st_size==sample['size']
  assert hashlib.sha256(path.read_bytes()).hexdigest()==sample['sha256'],str(path)
report=dict(utc=time.strftime('%FT%TZ',time.gmtime()),installed_images=images,processes=processes,
 existing_payload_samples_equal_on_both_mounts=samples,scope='Bounded final read check against prior accepted payload samples; not a complete before/after inventory')
(base/'final-service.json').write_text(json.dumps(report,indent=2)+'\n')
print('four installed daemons, two mounts, unchanged live images and',len(samples),'preserved payloads on both mounts PASS',flush=True)
