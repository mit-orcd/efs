"""Retained distributed W112 oracle on the real ORCD mounts (no private servers)."""
import importlib.util,sys,os,subprocess,json
from pathlib import Path
s=importlib.util.spec_from_file_location('o','/Users/mike/git/cluster/orcd-test.py');m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
m.OPTIONS += ['-o','ControlMaster=auto','-o','ControlPath=/private/tmp/efs-orcd-login-control3','-o','ControlPersist=600']
repo=Path('/Users/mike/Documents/Codex/2026-10-08/efs/work/devel-orcd')
rev=os.environ['ORCD_SOURCE_REV']
assert rev=='c2bfb9e9a281aae33dfaa01689e61866ccf60af8'
assert len(rev)==40 and all(c in '0123456789abcdef' for c in rev)
rev=rev[:12]+'-private'
m.MOUNT='/tmp/efs-c2bf-mount'
mode=os.environ['ORCD_PACK_MODE'];assert mode in ('0','1')
tag=f'{rev}-pack{mode}';phase=sys.argv[1];assert phase in ('create','warm','mutate','verify','unlink')
base=m.BULK+'/ordinary-overwrite-'+tag;folder=m.MOUNT+'/ordinary-overwrite-'+tag
common=f'''
from pathlib import Path
import hashlib,json,os
folder=Path({folder!r});base=Path({base!r});oracle=base/'oracle.json'
rows=json.loads(oracle.read_text()) if oracle.exists() else None
bodies=[b'ordinary-growth:'+bytes(range(256))*1024,b'full:'+bytes(range(256))*511+b'Z'*251,bytes(131072),b'partial replacement'*17]
assert len(bodies[1])==131072
'''
if phase=='create':
 code=common+'''
assert not oracle.exists() and not folder.exists();base.mkdir();folder.mkdir()
rows=[]
for i,body in enumerate(bodies):
 name='ordinary-overwrite-'+str(i);old=hashlib.sha256(name.encode()).digest()*16
 expected=body+old[len(body):] if len(body)<len(old) else body
 rows.append(dict(name=name,old_sha256=hashlib.sha256(old).hexdigest(),bytes=len(expected),expected_sha256=hashlib.sha256(expected).hexdigest()))
# Save intended bytes' identity before any filesystem mutation.
oracle.write_text(json.dumps(rows,indent=2))
with oracle.open('rb') as fd:os.fsync(fd.fileno())
for row in rows:
 old=hashlib.sha256(row['name'].encode()).digest()*16
 with (folder/row['name']).open('xb',buffering=0) as fd:
  assert fd.write(old)==512;os.fsync(fd.fileno());row['ino']=os.fstat(fd.fileno()).st_ino
oracle.write_text(json.dumps(rows,indent=2))
with oracle.open('rb') as fd:os.fsync(fd.fileno())
print(json.dumps(rows))
'''
elif phase=='warm':
 code=common+'''
for row in rows:
 path=folder/row['name'];assert path.stat().st_ino==row['ino'];assert hashlib.sha256(path.read_bytes()).hexdigest()==row['old_sha256']
print('warm peer old512 exact bytes/inodes PASS')
'''
elif phase=='mutate':
 code=common+'''
for row,body in zip(rows,bodies):
 with (folder/row['name']).open('r+b',buffering=0) as fd:
  assert fd.write(body)==len(body);os.fsync(fd.fileno())
for row in rows:
 path=folder/row['name'];got=path.read_bytes();assert path.stat().st_ino==row['ino'] and len(got)==row['bytes'] and hashlib.sha256(got).hexdigest()==row['expected_sha256'],row
print('primary growth/full/zero/partial prewrite oracle PASS')
'''
elif phase=='verify':
 code=common+'''
for row in rows:
 path=folder/row['name'];got=path.read_bytes();assert path.stat().st_ino==row['ino'] and len(got)==row['bytes'] and hashlib.sha256(got).hexdigest()==row['expected_sha256'],row
print('all4 expected postwrite bytes/inodes PASS')
'''
else:
 code=common+'''
for row in rows:(folder/row['name']).unlink()
folder.rmdir();print('all4 removed')
'''
host='fcstor009' if phase in ('warm','verify') else 'fcstor007'
result=m.shell(host,"python3 - <<'END'\n"+code+"\nEND",timeout=180)
out=repo/'results/measure/20261011-orcd-coalescing-rollout'/rev;out.mkdir(exist_ok=True)
(out/f'ordinary-pack{mode}-{phase}.log').write_text(result)
print(host,phase,result,flush=True)
