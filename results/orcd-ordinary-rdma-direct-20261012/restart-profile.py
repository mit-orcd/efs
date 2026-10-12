import importlib.util,json,shlex,concurrent.futures
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);m.OPTIONS+=['-o','ControlPath=/private/tmp/efs-orcd-login-control3']
p=Path('operations/orcd-packing-fix');meta=json.loads((p/'profile-stage.json').read_text());rev=meta['revision'];marker='.orcd-profile-'+rev[:12]
expected=json.loads((p/'profile-cold-expected.json').read_text())
body=f'''import os,pathlib,hashlib
p=pathlib.Path('/tmp/efs-mount/{marker}/ordinary-after-deploy')
b=b'ordinary unpacked post-deployment cold proof\\n'*13000
with p.open('wb') as f:f.write(b);f.flush();os.fsync(f.fileno())
print(hashlib.sha256(p.read_bytes()).hexdigest())
'''
expected['ordinary-after-deploy']=m.shell('fcstor007','python3 -c '+shlex.quote(body),90)
(p/'profile-restart-expected.json').write_text(json.dumps(expected,indent=2)+'\n')
print('Orderly final-source restart: seven client drains/unmounts, four server stops',flush=True)
m.stop()
code=(p/'deploy-profile.py').read_text();code=code[code.index('for i,h in enumerate(m.STORAGE,1):'):]
code=code.replace("['large','small']","['large','small','ordinary-after-deploy']").replace('profile-cold-results.json','profile-restart-cold-results.json').replace('DEPLOY PASS:','ORDERLY RESTART PASS:')
exec(compile(code,'profile-restart-start','exec'))
# The revised marker was written with packing disabled before redeployment;
# all three files therefore exercise ordinary object reclamation.
code=f'''import pathlib,json
p=pathlib.Path('/tmp/efs-mount/{marker}')
print(json.dumps({{n:(p/n).stat().st_ino for n in ['large','small','ordinary-after-deploy']}}))
'''
inodes=json.loads(m.shell('fcstor007','python3 -c '+shlex.quote(code),60))
def physical(host):
 code=f'''import pathlib,json
roots={m.STORES!r};inodes={inodes!r};out={{}}
for name,ino in inodes.items():
 seg=[]
 for _ in range(5):seg.append(f'{{ino%10000:04d}}');ino//=10000
 found=[]
 for root in roots:
  folder=pathlib.Path(root)/'data/exports/1'
  for part in reversed(seg):folder/=part
  found += [str(x) for x in folder.glob('*/*') if x.is_file()]
 out[name]=found
print(json.dumps(out))
'''
 return {'host':host,'files':json.loads(m.shell(host,'python3 -c '+shlex.quote(code),60))}
def scan():
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:return list(pool.map(physical,m.STORAGE))
before=scan()
assert all(any(row['files'][n] for row in before) for n in inodes),before
code=f'''import pathlib
p=pathlib.Path('/tmp/efs-mount/{marker}')
for n in {list(inodes)!r}:(p/n).unlink()
p.rmdir()
'''
m.shell('fcstor007','python3 -c '+shlex.quote(code),60)
import time
start=time.monotonic()
while True:
 after=scan()
 if all(not files for row in after for files in row['files'].values()):break
 assert time.monotonic()-start<180,after
 time.sleep(2)
(p/'profile-ordinary-gc.json').write_text(json.dumps({'inodes':inodes,'before':before,'after':after,'elapsed_seconds':time.monotonic()-start},indent=2)+'\n')
print('Ordinary object GC PASS: real fragments existed before unlink and were removed across all24 storage roots',flush=True)
