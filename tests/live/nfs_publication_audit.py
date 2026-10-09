#!/usr/bin/env python3
"""Audit W55 ledger through gateway bytes, authoritative rows and exact objects.
Read-only; requires files-engine store paths and explicit SSH destinations.
Inventory proves presence, not durability barriers or a storage-crash gate.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('ledger',type=Path)
p.add_argument('--key',required=True)
p.add_argument('--jump',required=True)
p.add_argument('--gateway',required=True)
p.add_argument('--nodes',nargs=3,required=True,
    help='SSH destinations for storage node IDs 1, 2 and 3, in that order')
p.add_argument('--store',default='/data1/data/exports/1')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(exist_ok=True)
ledger=json.loads(a.ledger.read_text()); files=ledger['files']
assert not ledger['errors'], ledger['errors']
assert ledger['operations_passed']==ledger['operations_expected'], ledger
assert files and len({f['name'] for f in files})==len(files)

def remote(host,code,payload):
    r=subprocess.run(['ssh','-i',a.key,'-J',a.jump,host,
        'python3 -c '+shlex.quote(code)],input=json.dumps(payload),
        text=True,capture_output=True,timeout=180)
    if r.returncode:
        raise RuntimeError(host+': '+r.stderr+r.stdout)
    return json.loads(r.stdout)

code='''import json,sys,pathlib,hashlib
x=json.load(sys.stdin); root=pathlib.Path('/mnt/efs')/pathlib.Path(x['directory']).name
out=[]
for f in x['files']:
 p=root/f['name']; b=p.read_bytes()
 out.append(dict(name=f['name'],inode=p.stat().st_ino,size=len(b),sha256=hashlib.sha256(b).hexdigest()))
print(json.dumps(out))
'''
view=remote(a.gateway,code,ledger)
(a.output/'gateway.json').write_text(json.dumps(view,indent=2)+'\n')
assert len(view)==len(files)
for f,v in zip(files,view):
    assert v['inode']==f['inode'] and v['size']==f['size'] and v['sha256']==f['sha256'],(f,v)
code='''import json,sys,subprocess
x=json.load(sys.stdin);out={}
for f in x:
 r=subprocess.run(['/tmp/efs/efs-mgmt','raft-getchunks','192.168.122.111:17432',str(f['inode'])],capture_output=True,text=True,timeout=30)
 if r.returncode: raise RuntimeError(r.stderr+r.stdout)
 out[str(f['inode'])]=r.stdout
print(json.dumps(out))
'''
rows=remote(a.nodes[0],code,files)
(a.output/'metadata.json').write_text(json.dumps(rows,indent=2)+'\n')
code='''import json,sys,pathlib
x=json.load(sys.stdin);out={}
for f in x['files']:
 ino=f['inode'];seg=['%04d'%((ino//(10000**i))%10000) for i in range(5)]
 root=pathlib.Path(x['store']).joinpath(*reversed(seg))
 out[str(ino)]=[str(p.relative_to(root)) for p in root.rglob('*') if p.is_file()]
print(json.dumps(out))
'''
inventories=[]
for n,host in enumerate(a.nodes,1):
    inv=remote(host,code,{'files':files,'store':a.store});inventories.append(inv)
    (a.output/('node%d.json'%n)).write_text(json.dumps(inv,indent=2)+'\n')
checked=0
for f in files:
    ino=str(f['inode']);ci=None;objects=[]
    assert 'status=0 ' in rows[ino],rows[ino]
    for line in rows[ino].splitlines()[1:]:
        m=re.search(r'ci=(\d+) nodes=([\d,]+) base_gen=(\d+)',line)
        if m:
            ci=int(m[1]);gen=int(m[3]);nodes=list(map(int,m[2].split(',')))
            if gen: objects.append((ci,gen,nodes))
        m=re.search(r'span off=\d+ len=(\d+) gen=(\d+) seq=\d+ nodes=([\d,]+)',line)
        if m and int(m[1]):objects.append((ci,int(m[2]),list(map(int,m[3].split(',')))))
    assert objects,(ino,'no live mapping')
    for ci,gen,nodes in objects:
        present=[]
        for fi,nid in enumerate(nodes):
            assert 0<=nid<=3,(ino,nodes)
            if nid and '%d/%d.%d.%d'%(ci>>10,ci,fi,gen) in inventories[nid-1][ino]:present.append(fi)
        assert len(present)>=2,(ino,ci,gen,nodes,present)
        checked+=1
summary={'files_verified':len(files),'live_objects_with_two_exact_fragments':checked,
         'boundary':'independent gateway bytes and current authoritative object presence; no storage restart'}
(a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary))
