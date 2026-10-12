import hashlib,json,tarfile,subprocess,time,traceback
from pathlib import Path
base=Path('/home/efs/nuc-c2-qualification'); out=base/'native';src=base/'source';out.mkdir()
try:
 expected={'efs-v030-c2bfb9e9-source.tar.gz':'4144442ef9c4733d254b7506496022ce614068eeb1eed452745fb7d61d6638ec','v030-c2bfb9e9-inputs.json':'e3eff745e0ea340763a9db11adbaae8da8b74d5b37a50a261c6b48cd8d1a64f2','v030-c2bfb9e9-dirs.json':'272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110'}
 for n,h in expected.items(): assert hashlib.sha256((base/n).read_bytes()).hexdigest()==h,n
 inputs=json.loads((base/'v030-c2bfb9e9-inputs.json').read_text());src.mkdir()
 with tarfile.open(base/'efs-v030-c2bfb9e9-source.tar.gz') as t:t.extractall(src,filter='data')
 for d in json.loads((base/'v030-c2bfb9e9-dirs.json').read_text()):(src/d).mkdir(parents=True,exist_ok=True)
 def verify():
  for n,h in inputs.items():assert hashlib.sha256((src/n).read_bytes()).hexdigest()==h,n
 verify();(out/'inputs-verified.json').write_text(json.dumps(dict(commit='c2bfb9e9a281aae33dfaa01689e61866ccf60af8',files=len(inputs),bundles=expected)))
 for mode in ('normal','fault'):
  ident='c2bfb9e9-release-'+mode
  flags=['EFS_GIT_ID='+ident,'EFS_GIT_DESCRIBE='+ident,'EFS_GIT_BRANCH=release-v0.3.0-pre-alpha','EFS_FAULTS='+str(int(mode=='fault'))]
  for phase,cmd in [('clean',['make','clean']),('all',['make','-j4','all',*flags]),('test',['make','-j4','test',*flags])]:
   name=mode+'-'+phase;(out/(name+'-start.txt')).write_text(time.strftime('%FT%TZ',time.gmtime()))
   with (out/(name+'.log')).open('w') as f:rc=subprocess.run(cmd,cwd=src,stdout=f,stderr=subprocess.STDOUT,timeout=3600).returncode
   (out/(name+'-exit.txt')).write_text(str(rc));assert rc==0,(name,rc)
  images={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench')}
  (out/(mode+'-images.json')).write_text(json.dumps(images,indent=2)); saved=base/(mode+'-images');saved.mkdir()
  import shutil
  for n in images:shutil.copy2(src/n,saved/n)
  verify()
 (out/'run-exit.txt').write_text('0')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());(out/'run-exit.txt').write_text('1');raise
