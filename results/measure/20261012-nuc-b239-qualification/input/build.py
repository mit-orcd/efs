import hashlib,json,tarfile,subprocess,time,traceback
from pathlib import Path
base=Path('/data1/efs/nuc-b239-qualification'); out=base/'native';src=base/'source';out.mkdir()
try:
 expected={'efs-v030-b239dc7d-source.tar.gz':'24f6ca3ec7d509a24c74de4b6a746d033bef872b7145eec9beb2e387a5caa548','v030-b239dc7d-inputs.json':'1500601db39a388c5d033c1ab8c9c537e0d94d5fc2c1a2ef9ed8946da90696c7','v030-b239dc7d-dirs.json':'911c85e28319837c62e30a5ab92fba394be649713ec3062b34442f58668197bb'}
 for n,h in expected.items(): assert hashlib.sha256((base/n).read_bytes()).hexdigest()==h,n
 inputs=json.loads((base/'v030-b239dc7d-inputs.json').read_text());src.mkdir()
 with tarfile.open(base/'efs-v030-b239dc7d-source.tar.gz') as t:t.extractall(src,filter='data')
 for d in json.loads((base/'v030-b239dc7d-dirs.json').read_text()):(src/d).mkdir(parents=True,exist_ok=True)
 def verify():
  for n,h in inputs.items():assert hashlib.sha256((src/n).read_bytes()).hexdigest()==h,n
 verify();(out/'inputs-verified.json').write_text(json.dumps(dict(commit='b239dc7de104ff8437e49d9333a529790601464c',files=len(inputs),bundles=expected)))
 for mode in ('normal','fault'):
  ident='b239dc7de104-release-'+mode
  flags=['EFS_GIT_ID='+ident,'EFS_GIT_DESCRIBE='+ident,'EFS_GIT_BRANCH=approved-merged','EFS_FAULTS='+str(int(mode=='fault'))]
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
