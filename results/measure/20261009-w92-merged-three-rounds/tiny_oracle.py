#!/usr/bin/env python3
"""Compare a completed tiny_files tree with scoped disk-catalog export/stats."""
import hashlib,json,os,subprocess,sys,time
from pathlib import Path
root=Path(sys.argv[1]); saved=Path(sys.argv[2]); phase=sys.argv[3]
expected=int(sys.argv[4]) if len(sys.argv)>4 else 12000
if phase=='capture':
    files={};dirs=0
    for base, children, names in os.walk(root):
        children[:]=[name for name in children if name!='.efs']
        dirs+=len(children)
        for name in names:
            path=Path(base)/name;files[str(path.relative_to(root))]=path.stat().st_size
    assert len(files)==expected,len(files)
    hashes={path:hashlib.sha256((root/path).read_bytes()).hexdigest() for path in sorted(files)[::max(1,expected//65)]}
    saved.write_text(json.dumps({'files':files,'dirs':dirs,'hashes':hashes},sort_keys=True))
model=json.loads(saved.read_text());export=saved.with_suffix('.jsonl')
for attempt in range(45):
    result=subprocess.run([sys.executable,str(root/'.efs/find'),'','--type','file','--json','--file',str(export)],capture_output=True,timeout=60)
    if result.returncode:
        time.sleep(1);continue
    rows=[json.loads(line) for line in export.read_text().splitlines()]
    actual={bytes.fromhex(row['entry']['path_hex']).decode():row['entry'] for row in rows if 'entry' in row}
    stats=subprocess.run([sys.executable,str(root/'.efs/stats'),'--json'],capture_output=True,timeout=60)
    if stats.returncode:time.sleep(1);continue
    stats=json.loads(stats.stdout)['stats']
    if set(actual)==set(model['files']) and stats['tree_files']==expected and stats['tree_dirs']==model['dirs'] and stats['tree_bytes']==sum(model['files'].values()):break
    time.sleep(1)
else:raise AssertionError((len(actual),stats))
assert rows[-1]=={'complete':True,'exported':expected},rows[-1]
for path,digest in model['hashes'].items():assert hashlib.sha256((root/path).read_bytes()).hexdigest()==digest,path
print(json.dumps({'phase':phase,'files':expected,'dirs':model['dirs'],'bytes':sum(model['files'].values()),'payload_samples':len(model['hashes']),'result':'PASS'}),flush=True)
