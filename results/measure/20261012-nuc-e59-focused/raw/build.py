import os,json,time,hashlib,subprocess,tarfile
from pathlib import Path
b=Path("/home/efs/nuc-e59-focus");o=b/'results';o.mkdir(exist_ok=True);s=b/'source-normal';s.mkdir(exist_ok=True)
with tarfile.open(b/'source.tar.gz') as t:t.extractall(s,filter='data')
manifest=json.loads((b/'inputs.json').read_text());assert manifest['commit']=='e59e815fab2916340baad585dc08712ed182b7ea';assert hashlib.sha256((b/'source.tar.gz').read_bytes()).hexdigest()==manifest['archive_sha256'];inputs=manifest['files']
for n,h in inputs.items():assert hashlib.sha256((s/n).read_bytes()).hexdigest()==h,n
env=dict(os.environ,EFS_GIT_ID='e59e815f-group3-normal',EFS_GIT_BRANCH='qualification-v030-tcp-direct',EFS_GIT_DESCRIBE='e59e815f-frozen')
cmd=['make','-j4','EFS_FAULTS=0','all','libefs.a'];start=time.time()
with (o/'build-complete-archive.log').open('w') as f:rc=subprocess.run(cmd,cwd=s,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
(o/'build.json').write_text(json.dumps(dict(command=cmd,exit=rc,seconds=time.time()-start),indent=2));(o/'build-exit.txt').write_text(str(rc));assert rc==0
images={}
for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench','libefs.a'):
 images[n]=dict(sha256=hashlib.sha256((s/n).read_bytes()).hexdigest())
 if n!='libefs.a':images[n]['version']=subprocess.run([str(s/n),'--version'],capture_output=True,text=True).stdout
(o/'normal-images.json').write_text(json.dumps(images,indent=2))
