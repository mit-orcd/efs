import subprocess,json,hashlib,os,socket,sys,time
from pathlib import Path
out={'utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'production':[],'mounts':[]}
for p in Path('/proc').glob('[0-9]*'):
 try:
  if p.stat().st_uid!=os.getuid():continue
  argv=[x.decode(errors='replace') for x in (p/'cmdline').read_bytes().split(b'\0') if x]
  if not argv:continue
  daemon=Path(argv[0]).name=='efsd' and '--storage' in argv and argv[argv.index('--storage')+1] in ('/data1/efs/n1','/data2/efs/n2','/data1/efs/n3','/data2/efs/n4')
  client=Path(argv[0]).name=='efs-fuse' and '/data1/efs/mnt' in argv
  if daemon or client:
   env={k.decode():v.decode() for kv in (p/'environ').read_bytes().split(b'\0') if b'=' in kv for k,v in [kv.split(b'=',1)] if k.startswith(b'EFS_')}
   out['production'].append({'pid':int(p.name),'argv':argv,'binary_sha256':hashlib.sha256((p/'exe').read_bytes()).hexdigest(),'environment':env})
 except (FileNotFoundError,ProcessLookupError,PermissionError):pass
out['production'].sort(key=lambda r:r['pid'])
out['installed']={n:hashlib.sha256((Path('/data1/efs/src')/n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench')}
out['version']=subprocess.check_output(['/data1/efs/src/efsd','--version'],text=True)
out['mounts']=[l for l in Path('/proc/mounts').read_text().splitlines() if 'fuse.efs-fuse' in l]
manifest=json.loads(Path(sys.argv[1]).read_text());bad=[]
for n,want in manifest.items():
 data=(Path('/data1/efs/mnt')/n).read_bytes()
 if len(data)!=want['size'] or hashlib.sha256(data).hexdigest()!=want['sha256']:bad.append(n)
out['payload_samples']={'paths':len(manifest),'mismatches':bad,'manifest_sha256':hashlib.sha256(Path(sys.argv[1]).read_bytes()).hexdigest()}
assert len(out['production'])==3 and not bad
print(json.dumps(out,indent=2))
