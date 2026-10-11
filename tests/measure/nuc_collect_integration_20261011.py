#!/usr/bin/env python3
"""Copy bounded controller/fixture evidence, retaining remote binary traces."""
import gzip,hashlib,json,re,shutil
from pathlib import Path
base=Path('/data1/efs/devel-nuc-20261011');out=base/'evidence-v3';out.mkdir(exist_ok=True)
folders=('build','candidate-build','fixed-build','fixed-live','fixed-v2-build','fixed-v2-live','fixed-v3-build','fixed-v3-live','control-build','control-v3-build','append-comparison','append-comparison-v3','block-traces','loaded-append-v2','loaded-append-notrun-plan20','focused','drive-policy','long-control-profile','block-traces-prebudget-plan','append-comparison-v3-prebudget-plan','loaded-append-v2-prebudget-plan','final-native','offline-append-fanout','commands-live')
fixtures={}
def copy(source,dest):
 dest.parent.mkdir(parents=True,exist_ok=True)
 if source.suffix in ('.log','.txt') and source.stat().st_size>65536:
  with source.open('rb') as inp,gzip.open(str(dest)+'.gz','wb') as target:shutil.copyfileobj(inp,target)
 else:shutil.copy2(source,dest)
for name in folders:
 folder=base/name
 if not folder.exists():continue
 for source in folder.rglob('*'):
  if not source.is_file():continue
  if source.suffix=='.data':
   copy_path=out/name/source.relative_to(folder).with_suffix('.data.sha256.json')
   copy_path.parent.mkdir(parents=True,exist_ok=True)
   copy_path.write_text(json.dumps(dict(remote_path=str(source),size_bytes=source.stat().st_size,sha256=hashlib.sha256(source.read_bytes()).hexdigest()))+'\n');continue
  if source.suffix not in ('.txt','.log','.json','.jsonl','.tsv','.diff'):continue
  copy(source,out/name/source.relative_to(folder))
  if source.suffix=='.log':
   for match in re.findall(r'^Evidence directory: (/data1/efs/gc-[a-z]+-[a-zA-Z0-9_-]+)$',source.read_text(errors='replace'),re.M):
    fixtures[Path(match).name]=Path(match)
for name,folder in fixtures.items():
 for source in folder.iterdir():
  if source.is_file() and source.suffix in ('.json','.tsv','.log','.txt'):
   copy(source,out/'fixtures'/name/source.name)
for source in base.iterdir():
 if source.is_file() and source.suffix in ('.py','.sh','.json','.log','.txt','.c'):
  copy(source,out/'controllers'/source.name)
for label,folder in (('baseline',base/'src'),('candidate',base/'candidate'),('control',base/'control')):
 paths=[p for parent in ('src','include','deps') for p in (folder/parent).rglob('*') if p.is_file() and p.suffix in ('.c','.h')]
 paths += [folder/'Makefile']
 (out/(label+'-source-sha256.json')).write_text(json.dumps({str(p.relative_to(folder)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},indent=2)+'\n')
print('collected',len(fixtures),'owned fixtures',flush=True)
