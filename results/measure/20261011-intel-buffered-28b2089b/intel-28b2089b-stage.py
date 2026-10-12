import os,sys,json,hashlib,tarfile,subprocess,time
from pathlib import Path
base=Path('/data1/efs/intel-qual-28b2089b');out=base/'evidence';out.mkdir(exist_ok=True)
manifest=json.loads((base/'inputs.json').read_text());expected='2ade76f798a2bbeac2f2647f6bd9b956a939a61126986deefc2c7c458e4f1454'
assert hashlib.sha256((base/'source.tar.gz').read_bytes()).hexdigest()==expected
proof={'source':'28b2089be6a5df835cc7bb46b9c2a9e2f10df190','files':len(manifest),'archive_sha256':expected,'variants':{}}
for variant in ('normal','fault'):
 root=base/variant;assert not root.exists(),root;root.mkdir()
 with tarfile.open(base/'source.tar.gz') as tf:
  for m in tf.getmembers():
   p=Path(m.name)
   assert not p.is_absolute() and '..' not in p.parts,m.name
  tf.extractall(root)
 bad=[n for n,h in manifest.items() if not (root/n).is_file() or hashlib.sha256((root/n).read_bytes()).hexdigest()!=h]
 assert not bad,bad[:10]
 proof['variants'][variant]={'all_input_hashes_match':True}
(out/'stage-proof.json').write_text(json.dumps(proof,indent=2))
for variant in ('normal','fault'):
 root=base/variant;faults='1' if variant=='fault' else '0'
 cmd=['make','-j4','EFS_FAULTS='+faults,'EFS_GIT_ID=28b2089b-release-'+variant,'EFS_GIT_BRANCH=release-v0.3.0-pre-alpha','EFS_GIT_DESCRIBE=v0.3.0-pre-alpha-candidate','efsd','efs-fuse','efs-mgmt','efs-bench','efs-query','libefs.a','tests/faults/fault_sim']
 t=time.monotonic();record={'command':cmd,'source':proof['source'],'started_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime())}
 with (out/(variant+'-build.log')).open('w') as log:r=subprocess.run(cmd,cwd=root,stdout=log,stderr=subprocess.STDOUT)
 record.update(returncode=r.returncode,seconds=time.monotonic()-t)
 if r.returncode==0:
  record['binaries']={n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ('efsd','efs-fuse','efs-mgmt','efs-bench','efs-query','libefs.a')}
  record['versions']={n:subprocess.check_output([str(root/n),'--version'],text=True) for n in ('efsd','efs-fuse','efs-mgmt')}
 (out/(variant+'-build.json')).write_text(json.dumps(record,indent=2));print(variant,record['returncode'],round(record['seconds'],2),flush=True)
 assert r.returncode==0,variant
(out/'builds.finished').write_text(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()))
