#!/usr/bin/env python3
"""Verify completed private stores independently of sampled GC counters."""
import argparse,hashlib,json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('root',type=Path)
a=p.parse_args();proof=[]
for case in sorted(a.root.iterdir()):
    marker=case/('exit-code' if case.name.startswith('r') else '../'+case.name+'.exit')
    if not marker.is_file() or marker.read_text().strip()!='0':continue
    records=list(case.glob('gc-*/tiny-mixed.json'))
    if not records:continue
    assert len(records)==1,records
    path=records[0];manifest=path.parent/'manifest.json'
    roots=json.loads(manifest.read_text())['real_roots'];assert len(roots)==4
    inventories=[]
    for root in roots:
        files=[str(f) for f in (Path(root)/'data/exports/1').rglob('*') if f.is_file()]
        inventories.append(dict(root=root,physical_files=len(files)))
        assert not files,(case,root,files[:10])
    proof.append(dict(case=case.name,manifest_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest(),
        result_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),inventories=inventories))
assert proof,'no completed cases'
(a.root/'physical-inventory-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
print('PHYSICAL INVENTORY EMPTY:',len(proof),'completed cases')
