#!/usr/bin/env python3
"""Decode stopped 64-file A/B stores; retain CRC-validated committed identities."""
import argparse,collections,json,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',required=True,type=Path)
p.add_argument('--root',required=True,type=Path)
a=p.parse_args()
sys.path.insert(0,str(a.source/'tests/live'))
from raft_tail import commands,applied_index
proof={}
for variant in ('separate','combined'):
    records=list((a.root/('trace-'+variant)).glob('gc-*/tiny-mixed.json'))
    assert len(records)==1,records
    fixture=records[0].parent
    assert json.loads(records[0].read_text())['tiny_files']==64
    node=fixture/'n2/mdraft'
    bounds={group:applied_index(node/f'applied.{group}') for group in (0,2)}
    leases=collections.defaultdict(list)
    for (group,index),(term,command) in commands(node/'log/raft.log',bounds).items():
        if len(command)==66 and command[:2]==bytes([9,16]):
            ino=int.from_bytes(command[38:46],'big')
            flags=int.from_bytes(command[54:58],'big')
            leases[ino].append(dict(group=group,index=index,term=term,flags=flags,command=command.hex()))
    adopted={str(ino):edges for ino,edges in leases.items() if any(edge['flags']&4 for edge in edges)}
    expected=(1,1,2,2,4) if variant=='separate' else (1,2,2,5)
    counts=collections.Counter(tuple(sorted(edge['flags'] for edge in edges)) for edges in adopted.values())
    assert len(adopted)==64 and counts=={expected:64},(variant,len(adopted),counts)
    proof[variant]=dict(source_log=str(node/'log/raft.log'),applied_bounds=bounds,files=64,
        creation_lease_entries_per_file=3 if variant=='separate' else 2,
        cold_read_additional_open_close_per_file=2,committed_edges=adopted)
(a.root/'lease-consensus-proof.json').write_text(json.dumps(proof,indent=2)+'\n')
print('COMMITTED CRC-VALID LEASE COUNT PROOF PASS')
