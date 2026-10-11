#!/usr/bin/env python3
"""CRC/applied-bounded lease evidence from a stopped private guard fixture."""
import argparse,json,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,required=True)
p.add_argument('--fixture',type=Path,required=True)
p.add_argument('--ino',type=int,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();sys.path.insert(0,str(a.source/'tests/live'))
from raft_tail import commands
manifest=json.loads((a.fixture/'manifest.json').read_text())
node=Path(manifest['roots'][1])/'mdraft'
bounds={g:int.from_bytes((node/f'applied.{g}').read_bytes(),'big') for g in (0,2)}
edges=[]
for (group,index),(term,cmd) in commands(node/'log/raft.log',bounds).items():
    if len(cmd)==66 and cmd[0]==9 and cmd[1] in (12,16) and int.from_bytes(cmd[38:46],'big')==a.ino:
        edges.append(dict(group=group,index=index,term=term,uuid=cmd[2:18].hex(),
                          seq=int.from_bytes(cmd[22:30],'big'),
                          generation=int.from_bytes(cmd[46:54],'big'),
                          flags=int.from_bytes(cmd[54:58],'big'),command=cmd.hex()))
assert any(e['flags']==5 for e in edges),edges
a.output.write_text(json.dumps(dict(ino=a.ino,source_log=str(node/'log/raft.log'),
                                  applied_bounds=bounds,committed_edges=edges),indent=2)+'\n')
print('Applied CRC-valid guard-inode combined-adoption edge verified')
