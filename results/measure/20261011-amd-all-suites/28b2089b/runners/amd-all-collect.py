from pathlib import Path
import os,shutil,json,hashlib
root=Path('/data1/efs/release-v030-results-20261012-28b2089b');dest=root/'compact';dest.mkdir(exist_ok=True)
skip={'compact','real-lsm-sanitizer-src','fault-live-src','normal-binaries','fault-binaries','n1','n2','n3','n4','n0','mdraft','data','meta','kv','deps','src','include','__pycache__'}
chosen=[]
for parent,dirs,files in os.walk(root):
 dirs[:]=[d for d in dirs if d not in skip]
 for n in files:
  p=Path(parent)/n
  if p.suffix not in {'.log','.tsv','.json','.jsonl','.exit','.txt','.out','.vg','.finished'} and n not in {'PASS','FAIL','finished'}:continue
  # Only test journals, never arbitrary test payloads or compiled artifacts.
  if p.stat().st_size>80*1024*1024:continue
  rel=p.relative_to(root);to=dest/rel;to.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,to);chosen.append(rel.as_posix())
for p in Path('/tmp').glob('efs-all-28b2089b*'):
 if p.is_dir():
  for parent,dirs,files in os.walk(p):
   dirs[:]=[d for d in dirs if d not in skip]
   for n in files:
    f=Path(parent)/n
    if f.suffix not in {'.log','.tsv','.json','.jsonl','.exit','.txt','.out','.vg'} and n not in {'PASS','FAIL'}:continue
    if f.stat().st_size>80*1024*1024:continue
    rel=Path('tmp-fixtures')/p.name/f.relative_to(p);to=dest/rel;to.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(f,to)
 elif p.suffix in {'.out','.vg'}:
  to=dest/'tmp-fixtures'/p.name;to.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(p,to)
manifest={p.relative_to(dest).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dest.rglob('*')) if p.is_file() and p.name!='sha256.json'}
(dest/'sha256.json').write_text(json.dumps(manifest,indent=2)+'\n');print(json.dumps({'files':len(manifest),'bytes':sum((dest/p).stat().st_size for p in manifest)}))
