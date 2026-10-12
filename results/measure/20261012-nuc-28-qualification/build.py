import os,json,time,hashlib,subprocess,tarfile,traceback
from pathlib import Path
b=Path('/home/efs/nuc-28-qualification');o=b/'results';o.mkdir(exist_ok=True)
m=json.loads((b/'inputs.json').read_text())
def verify(s):
 for n,h in m.items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
try:
 assert hashlib.sha256((b/'source.tar.gz').read_bytes()).hexdigest()=='2ade76f798a2bbeac2f2647f6bd9b956a939a61126986deefc2c7c458e4f1454'
 for mode in ('normal','fault'):
  s=b/('source-'+mode);s.mkdir()
  with tarfile.open(b/'source.tar.gz') as t:t.extractall(s,filter='data')
  verify(s)
  env=dict(os.environ,EFS_GIT_ID='28b2089b-release-'+mode,EFS_GIT_BRANCH='release-v0.3.0-pre-alpha',EFS_GIT_DESCRIBE='v0.3.0-pre-alpha-candidate')
  cmds=[['make','clean'],['make','-j8','EFS_FAULTS='+('1' if mode=='fault' else '0'),'all','libefs.a','tests/stress/pack_storage_soak']]
  rows=[]
  for i,cmd in enumerate(cmds):
   st=time.time()
   with (o/(mode+'-build-'+str(i)+'.log')).open('w') as f:rc=subprocess.run(cmd,cwd=s,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
   rows.append(dict(command=cmd,returncode=rc,seconds=time.time()-st))
   (o/(mode+'-build.json')).write_text(json.dumps(rows,indent=2));assert rc==0
  verify(s)
  images={}
  for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench','libefs.a'):
   images[n]=dict(sha256=hashlib.sha256((s/n).read_bytes()).hexdigest())
   if n!='libefs.a':images[n]['version']=subprocess.run([str(s/n),'--version'],capture_output=True,text=True).stdout
  (o/(mode+'-images.json')).write_text(json.dumps(images,indent=2))
 (o/'build-exit.txt').write_text('0')
except BaseException:
 (o/'build-failure.txt').write_text(traceback.format_exc());(o/'build-exit.txt').write_text('1');raise
