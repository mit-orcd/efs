#!/usr/bin/env python3
"""Export the complete committed AN source, including full-gate assets.

No path filter: docs, tools and tracked test fixtures are gate dependencies.
Exports Git objects only, never working-tree stores, logs or generated payloads.
The destination archive and receipt must be new paths outside the repository.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--commit',required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args()
revision=subprocess.check_output(['git','rev-parse','--verify',a.commit+'^{commit}'],cwd=root,text=True).strip()
assert len(revision)==40 and all(c in '0123456789abcdef' for c in revision)
out=a.out.resolve();receipt=out.with_suffix(out.suffix+'.json')
assert out.suffix=='.tar' and not out.is_relative_to(root)
assert out.parent.is_dir() and not out.exists() and not receipt.exists()
# Exclusive create also prevents a race from replacing somebody else's archive.
with out.open('xb') as f:
    subprocess.run(['git','archive','--format=tar',revision],cwd=root,stdout=f,check=True)
h=hashlib.sha256()
with out.open('rb') as f:
    for block in iter(lambda:f.read(1<<20),b''):h.update(block)
receipt.write_text(json.dumps(dict(commit=revision,archive=str(out),bytes=out.stat().st_size,
                                 sha256=h.hexdigest(),scope='complete committed Git tree; no path filter'),indent=2)+'\n')
print(receipt.read_text(),end='')
