"""Retain small raw gate artefacts, never copy or remove service payload stores."""
import gzip,json,re
from pathlib import Path
base=Path(__file__).resolve().parent
out=base/'fixtures';out.mkdir(exist_ok=True)
roots=set()
for log in base.rglob('*.log'):
 for m in re.finditer(r'/data[12]/efs/(gc-direct-[A-Za-z0-9_-]+)',log.read_text(errors='replace')):
  roots.add(Path('/data1/efs')/m[1]);roots.add(Path('/data2/efs')/m[1])
manifest=[]
for root in sorted(roots):
 if not root.is_dir():continue
 dst=out/root.name;dst.mkdir(exist_ok=True)
 retained=[]
 for p in root.iterdir():
  if not p.is_file():continue
  if p.suffix in ('.json','.tsv','.txt','.c') and p.stat().st_size<4<<20:
   (dst/p.name).write_bytes(p.read_bytes());retained.append(p.name)
  elif p.suffix=='.log':
   with gzip.open(dst/(p.name+'.gz'),'wb') as f:f.write(p.read_bytes())
   retained.append(p.name+'.gz')
 manifest.append(dict(root=str(root),retained=retained))
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
print(json.dumps(manifest,indent=2))
