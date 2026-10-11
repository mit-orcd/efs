#!/usr/bin/env python3
"""Build the frozen W97 root barrier overlay after W52 timed and cold gates."""
import hashlib,json,subprocess,tarfile,time,traceback
from pathlib import Path

base=Path('/data1/efs/devel-nuc-20261011');out=base/'root-scope-build';out.mkdir(exist_ok=False)
src=base/'root-scope-src'
try:
 deadline=time.monotonic()+18000
 while not (base/'clock-cold/run-exit.txt').exists():
  assert time.monotonic()<deadline,'clock cold prerequisite wait timed out'
  for phase in ('clock-build','clock-live','clock-install','w52-clock-20000','clock-cold'):
   assert not (base/phase/'failure.txt').exists(),phase+' failed; no build accepted'
   assert not (base/phase/'blocked-prerequisite.json').exists(),phase+' was not run'
  time.sleep(1)
 assert (base/'clock-cold/run-exit.txt').read_text().strip()=='0'
 assert (base/'root-scope-focused/run-exit.txt').read_text().strip()=='0'
 archive=base/'efs-root-scope-766814c8.tar'
 assert hashlib.sha256(archive.read_bytes()).hexdigest()=='5a93449aee83416c535b3d622c5f7f91d0aa8e156a05d9fabe27af5466943c5b'
 assert not src.exists()
 subprocess.run(['rsync','-a','--exclude=/results/',str(base/'clock-src')+'/',str(src)+'/'],check=True)
 with tarfile.open(archive) as tar:
  assert set(tar.getnames())=={'src/server/store.c','src/server/packed_fence.h','tests/test_store_durability.c'}
  tar.extractall(src,filter='data')
 expected=json.loads((base/'clock-core-expected.json').read_text())
 focused=json.loads((base/'root-scope-focused/provenance.json').read_text())['sources']
 expected.update({p:d for p,d in focused.items() if p.startswith('src/')})
 current={str(p.relative_to(src)):hashlib.sha256(p.read_bytes()).hexdigest() for folder in ('src','include') for p in (src/folder).rglob('*') if p.suffix in ('.c','.h')}
 current['Makefile']=hashlib.sha256((src/'Makefile').read_bytes()).hexdigest()
 assert current==expected,'unexpected root candidate core changes'
 evidence=base/'clock-src/results';(src/'results').mkdir()
 subprocess.run(['rsync','-a','--link-dest='+str(evidence),str(evidence)+'/',str(src/'results')+'/'],check=True)
 versions=['EFS_GIT_ID=766814c8-root-nuc','EFS_GIT_DESCRIBE=766814c8-root-nuc','EFS_GIT_BRANCH=devel-nuc']
 (out/'provenance.json').write_text(json.dumps(dict(core='f52cd2dd + 19cbb11f + 766814c8',overlay_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),core_sha256=current,limits='Isolated clean/native acceptance only; no installed images changed, no measured benefit.'),indent=2))
 for name,command in (('clean',['make','clean']),('all',['make','-j4','all',*versions]),('native',['make','-j4','test',*versions])):
  with (out/(name+'.log')).open('w') as log:rc=subprocess.run(command,cwd=src,stdout=log,stderr=subprocess.STDOUT,timeout=1800).returncode
  (out/(name+'-exit.txt')).write_text(str(rc)+'\n');assert rc==0,(name,rc)
 images={n:dict(sha256=hashlib.sha256((src/n).read_bytes()).hexdigest(),version=subprocess.check_output([str(src/n),'--version'],text=True)) for n in ('efsd','efs-fuse','efs-mgmt','efs-query','efs-bench')}
 (out/'images.json').write_text(json.dumps(images,indent=2));(out/'run-exit.txt').write_text('0\n')
except BaseException:
 (out/'failure.txt').write_text(traceback.format_exc());raise
