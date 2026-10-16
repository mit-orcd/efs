"""Owned mounted shared-fragment proof; scans every active storage root."""
import importlib.util,subprocess,sys,shlex,json,concurrent.futures,time,os
from pathlib import Path
s=importlib.util.spec_from_file_location('orcd','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
m.OPTIONS += ['-o','ControlMaster=auto','-o','ControlPath=/private/tmp/efs-orcd-login-control3','-o','ControlPersist=600']
repo=Path('/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd')
rev='c2bfb9e9a281-private'
m.MOUNT='/tmp/efs-c2bf-mount'
m.STORES=[f'/data1/{i:02}/efs-orcd-c2bf-private' for i in range(1,7)]
source=m.BULK+'/shared-gc-'+rev;fixture=source+'/fixture.json';directory=m.MOUNT+'/shared-gc-'+rev
out=repo/'results/measure/20261011-orcd-coalescing-rollout'/rev;out.mkdir(parents=True,exist_ok=True)
def run(host,code,timeout=180):return m.shell(host,'python3 -c '+shlex.quote(code),timeout=timeout)
def load():return json.loads(run('fcstor007',f"from pathlib import Path;print(Path({fixture!r}).read_text())"))
def physical(o):
 def one(host):
  code=f'''from pathlib import Path
import json
roots={m.STORES!r};relative={o['relative']!r};key=({o['pack_id']},{o['generation']});fragments=[];fences=[]
for root in roots:
 d=Path(root)/'data/packs-v1/1'/relative
 for p in d.glob('*'):
  if p.is_file():fragments.append(str(p))
 p=Path(root)/'meta/pack-fences-v1/1'/str(key[0])/str(key[1])
 if p.exists():
  b=p.read_bytes()
  fences.append({{'path':str(p),'magic':b[:8].decode(),'bytes':len(b),
   'valid_identity':len(b)==64 and b[:8]==b'EFSDEAD1' and int.from_bytes(b[8:12],'big')==1 and
      int.from_bytes(b[16:24],'big')==key[0] and int.from_bytes(b[24:32],'big')==key[1]}})
print(json.dumps({{'host':{host!r},'fragments':fragments,'fences':fences}}))'''
  return json.loads(run(host,code,60))
 with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(one,m.STORAGE))
 return {'nodes':rows,'total':sum(len(r['fragments']) for r in rows)}
action=sys.argv[1]
if action=='create':
 print(run('fcstor007',f'''from pathlib import Path
import concurrent.futures,json,hashlib
root=Path({directory!r});root.mkdir();Path({source!r}).mkdir()
def write(i):
 p=root/str(i);b=bytes([i])*512;expected=hashlib.sha256(b).hexdigest()
 with p.open('wb') as f:assert f.write(b)==len(b)
 assert p.read_bytes()==b
 return {{'path':str(p),'ino':p.stat().st_ino,'sha256':expected,'bytes':len(b)}}
with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:files=list(pool.map(write,range(64)))
Path({source!r}+'/files.json').write_text(json.dumps(files));print('64 private TCP/direct mounted exact small-file reads PASS')'''),flush=True)
 files=json.loads(run('fcstor007',f'from pathlib import Path;print(Path({source!r}+"/files.json").read_text())'));wanted={f['ino'] for f in files};objects=[]
 for host in m.STORAGE:
  code=f'''from pathlib import Path
import json
out=[];wanted=set({sorted(wanted)!r})
for root in {m.STORES!r}:
 base=Path(root)/'data/packs-v1/1'
 for p in base.rglob('0'):
  if not p.is_file():continue
  blob=p.read_bytes();off=blob.find(b'EFSPACK1')
  if not 0<=off<=64:continue
  b=blob[off:];n=int.from_bytes(b[24:28],'big')
  if n<2 or n>512:continue
  inos=[int.from_bytes(b[96+72*i:104+72*i],'big') for i in range(n)]
  if len(inos)!=len(set(inos)) or not set(inos)<=wanted:continue
  out.append({{'relative':str(p.parent.relative_to(base)),'count':n,'inos':inos,'pack_id':int.from_bytes(b[32:40],'big'),'generation':int.from_bytes(b[40:48],'big'),'header_host':{host!r},'header_store':root}})
print(json.dumps(out))'''
  objects.extend(json.loads(run(host,code,90)))
 assert objects,'No multi-record packed object found; storage path is not demonstrated'
 selected=None
 for candidate in sorted(objects,key=lambda x:x['count'],reverse=True):
  possible=[f for f in files if f['ino'] in candidate['inos']]
  checks=[];actual=[]
  for f in possible:
   command=[m.BULK+'/qualification-c2bfb9e9a281/shared-mapping-proof',str(f['ino']),str(candidate['pack_id']),str(candidate['generation']),'19862']
   code=f"import subprocess,json;p=subprocess.run({command!r},text=True,capture_output=True);print(json.dumps({{'rc':p.returncode,'output':p.stdout,'error':p.stderr}}))"
   check=json.loads(run('fcstor007',code,90));checks.append(dict(check,ino=f['ino']))
   if check['rc']==0:actual.append(f)
  if len(actual)>=2:
   selected=dict(candidate,files=actual,mapping_checks=checks,live_count=len(actual));break
 assert selected,'No selected object has two freshly proved canonical live mappings'
 o=selected
 o['all_files']=files;o['removed']=[];o['candidate_objects']=objects
 print(run('fcstor007',f'from pathlib import Path;Path({fixture!r}).write_text({json.dumps(o)!r});print("selected exact shared object")'),flush=True)
 p=physical(o);assert p['total']==3,p;o['initial_physical']=p
 (out/'shared-gc-fixture.json').write_text(json.dumps(o,indent=2)+'\n');print(json.dumps(o),flush=True)
elif action=='verify-all':
 o=load();removed={f['ino'] for f in o['removed']}
 for host in m.CLIENTS:
  print(run(host,f"from pathlib import Path;import hashlib;rows={o['all_files']!r};removed={removed!r};[None if f['ino'] in removed else (Path(f['path']).stat().st_ino==f['ino'] and hashlib.sha256(Path(f['path']).read_bytes()).hexdigest()==f['sha256']) or (_ for _ in ()).throw(AssertionError(f)) for f in rows];print('all surviving fixture inode/bytes PASS')"),flush=True)
elif action=='release-all':
 o=load()
 print(run('fcstor007',f"from pathlib import Path;rows={o['all_files']!r};[(Path(f['path']).unlink() if Path(f['path']).exists() else None) for f in rows];print('entire64file fixture removed')"),flush=True)
elif action=='wait-all-deleted':
 o=load();objects={ (c['pack_id'],c['generation']):c for c in o['candidate_objects'] };end=time.monotonic()+180
 while True:
  rows=[physical(c) for c in objects.values()]
  if all(p['total']==0 and all(any(f['valid_identity'] for f in n['fences']) for n in p['nodes']) for p in rows):break
  assert time.monotonic()<end,('fixture pack physical deletion exceeded180s',rows)
  time.sleep(2)
 (out/'shared-gc-all-objects-deleted.json').write_text(json.dumps(rows,indent=2)+'\n');print('all fixture candidate objects physically absent PASS',flush=True)
elif action=='verify-all-objects-deleted':
 o=load();objects={(c['pack_id'],c['generation']):c for c in o['candidate_objects']};rows=[physical(c) for c in objects.values()]
 assert all(p['total']==0 and all(any(f['valid_identity'] for f in n['fences']) for n in p['nodes']) for p in rows),rows
 (out/'shared-gc-all-objects-cold-deleted.json').write_text(json.dumps(rows,indent=2)+'\n');print('all captured fixture objects absent and all4 durable fences PASS',flush=True)
elif action=='verify-all-absent':
 o=load()
 for host in m.CLIENTS:
  print(run(host,f"import os,errno;assert os.path.isdir({directory!r});rows={o['all_files']!r};\nfor f in rows:\n try:os.lstat(f['path'])\n except OSError as e:assert e.errno==errno.ENOENT,(f,e)\n else:raise AssertionError(f)\nprint('all64 fixture paths authoritativeENOENT PASS')"),flush=True)
elif action=='release-one':
 print(run('fcstor007',f'''from pathlib import Path
import json,hashlib
o=json.loads(Path({fixture!r}).read_text());f=o['files'].pop(0);Path(f['path']).unlink();o['removed'].append(f)
for f in o['files']:assert hashlib.sha256(Path(f['path']).read_bytes()).hexdigest()==f['sha256']
Path({fixture!r}).write_text(json.dumps(o));print('partial unlink retains exact shared neighbors PASS')'''),flush=True)
 o=load();p=physical(o);assert p['total']==3,p;(out/'shared-gc-partial.json').write_text(json.dumps(p,indent=2)+'\n')
elif action=='verify-survivors':
 print(run('fcstor007',f'''from pathlib import Path
import json,hashlib
o=json.loads(Path({fixture!r}).read_text())
for f in o['files']:
 p=Path(f['path']);assert p.stat().st_ino==f['ino'];assert hashlib.sha256(p.read_bytes()).hexdigest()==f['sha256']
print('cold shared neighbors exact inode and expected byte hashes PASS')'''),flush=True)
 p=physical(load());assert p['total']==3,p;print(json.dumps(p),flush=True)
elif action=='release-rest':
 print(run('fcstor007',f'''from pathlib import Path
import json
o=json.loads(Path({fixture!r}).read_text())
for f in o['files']:Path(f['path']).unlink()
o['removed']+=o['files'];o['files']=[];Path({fixture!r}).write_text(json.dumps(o));print('last shared references unlinked')'''),flush=True)
elif action in ('verify-retained','verify-deleted','fences'):
 o=load();p=physical(o)
 if action=='verify-retained':assert p['total']==3,p
 if action=='verify-deleted':assert p['total']==0,p
 (out/('shared-gc-'+action+'.json')).write_text(json.dumps(p,indent=2)+'\n');print(json.dumps(p),flush=True)
elif action in ('wait-fences','wait-deleted'):
 o=load();end=time.monotonic()+180
 while True:
  p=physical(o)
  valid={r['host']:any(f['valid_identity'] for f in r['fences']) for r in p['nodes']}
  if action=='wait-fences':
   assert p['total']==3,('deleted before captured offline member fence',p)
   assert not valid[m.STORAGE[3]],('offline member unexpectedly fenced',p)
   done=all(valid[h] for h in m.STORAGE[:3])
  else:done=p['total']==0 and all(valid.values())
  if done:break
  assert time.monotonic()<end,('GC gate exceeded unchanged 180s deadline',p)
  time.sleep(2)
 (out/('shared-gc-'+action+'.json')).write_text(json.dumps(p,indent=2)+'\n')
 print(action+' PASS '+json.dumps(p),flush=True)
else:raise SystemExit('unknown action')
