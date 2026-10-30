#!/usr/bin/env python3
"""Copy closed BB text/results/captures to a fresh on-host evidence directory.

Never traverses store or mount directories; never prunes originals. Keep those
while the audit's integration review is open. Run only after the controllers
have exited. SHA256SUMS describes the copied files, independently re-read.
"""
import argparse, hashlib, json, os, shutil, socket
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('private',type=Path);p.add_argument('launch_logs',type=Path)
p.add_argument('destination',type=Path);p.add_argument('--orcd',action='store_true')
a=p.parse_args();root=a.private.resolve();logs=a.launch_logs.resolve();dest=a.destination.absolute()
assert root.stat().st_uid==os.getuid() and not dest.exists()
assert root.name.startswith('efs-taskBB-orcd-') if a.orcd else root.name=='efs-taskBB-20261029'
selected={}
def add(path,label):
 if not path.exists():return
 assert path.is_file() and not path.is_symlink(),path
 selected[label]=path

def direct(directory,prefix):
 if directory.is_dir():
  for f in directory.iterdir():
   if f.is_file() and not f.is_symlink() and f.suffix in ('.log','.json','.jsonl','.txt','.tsv','.bin'):add(f,prefix+'/'+f.name)

def matrix(directory,prefix):
 direct(directory,prefix)
 for f in (directory/'cluster').glob('*.log'):add(f,prefix+'/cluster/'+f.name)

def closed(directory):
 state=directory/'state.json'
 assert state.is_file(),state
 d=json.loads(state.read_text());assert d['verdict'] in ('FAIL','PASS_SCOPED','PASS_EXECUTED_WITH_HOST_LIMITS','PASS_SCOPED_OBSERVED_QUIET','PASS_SCOPED_SINGLE_HOST_HCA','BUILD_COMPLETE_NOT_QUALIFIED'),state
 return d

if a.orcd:
 for stage in ('build','qualify'):
  d=root/stage
  if d.exists():
   closed(d);direct(d,stage)
   for transport in ('tcp','rdma'):matrix(d/('matrix-'+transport),stage+'/matrix-'+transport)
 for side in ('ga','gb'):
  for case in (root/side).glob('gc-*'):direct(case,side+'/'+case.name)
else:
 for d in sorted(root.glob('qualification-*')):
  if d.is_dir():
   closed(d);direct(d,d.name);matrix(d/'matrix',d.name+'/matrix')
   if (d/'candidate').is_dir():
    closed(d/'candidate');direct(d/'candidate',d.name+'/candidate');matrix(d/'candidate/matrix',d.name+'/candidate/matrix')
   for side in ('base-a','base-b'):
    for case in (d/side).glob('gc-*'):direct(case,d.name+'/'+side+'/'+case.name)
 for d in sorted(root.glob('base-matrix-*')):matrix(d,d.name)
 for d in sorted(root.glob('source-*')):
  for f in d.iterdir():
   if f.is_file() and f.suffix in ('.txt','.tsv'):add(f,d.name+'/'+f.name)
  for f in (d/'tmp').glob('fail-*.log'):add(f,d.name+'/tmp/'+f.name)
 for d in root.glob('gc-*'):
  for case in d.glob('gc-*'):direct(case,d.name+'/'+case.name)
 for d in root.glob('cold-race-negative-*'):
  add(d/'test.c',d.name+'/test.c')
# Only finalized launcher files, not arbitrary nested private fixtures.
for f in logs.iterdir():
 if f.is_file() and f.suffix in ('.log','.json',''):
  add(f,'launch/'+f.name)
assert selected
# All source inputs are closed; capture bytes, fsync, then compare by independent
# re-read before publishing a receipt. Failure leaves a new incomplete capture.
dest.mkdir(parents=True);rows=[]
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):h.update(b)
 return h.hexdigest()
for name,src in sorted(selected.items()):
 target=dest/name;target.parent.mkdir(parents=True,exist_ok=True)
 before=digest(src);shutil.copyfile(src,target)
 with target.open('rb') as f:os.fsync(f.fileno())
 assert digest(src)==before==digest(target),(src,'changed or copy mismatch')
 rows.append(dict(path=name,source=str(src),bytes=target.stat().st_size,sha256=before))
manifest=dest/'SHA256SUMS';manifest.write_text(''.join(x['sha256']+'  '+x['path']+'\n' for x in rows))
receipt=dest/'receipt.json';receipt.write_text(json.dumps(dict(host=socket.gethostname(),files=len(rows),manifest_sha256=digest(manifest),entries=rows,originals_retained=True,stores_not_copied=True),indent=2)+'\n')
for f in (manifest,receipt):
 with f.open('rb') as fd:os.fsync(fd.fileno())
# Directory durability for the complete newly created capture tree.
for d in sorted([x for x in dest.rglob('*') if x.is_dir()]+[dest,dest.parent],key=lambda x:len(x.parts),reverse=True):
 fd=os.open(d,os.O_RDONLY|os.O_DIRECTORY)
 try:os.fsync(fd)
 finally:os.close(fd)
print(json.dumps(dict(destination=str(dest),files=len(rows),manifest_sha256=digest(manifest),receipt_sha256=digest(receipt))))
