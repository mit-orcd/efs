#!/usr/bin/env python3
"""scripts/b_checks.py (alpha 1.0 B1-B5 driver): its parsers and checks on
canned efs-mgmt output and synthetic fragment records, with negative
controls, so a wrong parse cannot turn a cluster run into a false PASS."""
import importlib.util, struct, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('b_checks', root / 'scripts/b_checks.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)

STATUS = '''Cluster nodes (4):
  node 1: 127.0.0.1:40001  up  used=1.5 MiB  quota=1.0 GiB (0.1%)  storage(1):
      bytes: used=1572864 quota=1073741824 capacity=1073741824 available=1072168960 valid=1
      /tmp/n1
  node 2: 127.0.0.1:40002  up  used=0 B  quota=1.0 GiB (0.0%)  storage(1):
      bytes: used=0 quota=1073741824 capacity=1073741824 available=1073741824 valid=1
      /tmp/n2
  node 3: 127.0.0.1:40003  up  used=0 B  quota=1.0 GiB (0.0%)  storage(1):
      bytes: used=0 quota=1073741824 capacity=1073741824 available=1073741824 valid=0
      /tmp/n3
  node 4: 127.0.0.1:40004  DOWN  used=0 B  quota=1.0 GiB (0.0%)  storage(1):
      /tmp/n4
Usable (2+1 logical): used=1.0 MiB  free=2.0 GiB  capacity=2.0 GiB (0.1%; filesystem capacity)
Usable bytes: used=1048576 free=2146435072 capacity=2147483648
Cluster state: DEGRADED (3 up, 1 down)
'''
st = b.parse_status(STATUS)
assert sorted(st['members']) == [1, 2, 3, 4], st
assert st['members'][1]['used'] == 1572864 and st['members'][1]['valid'] is True
assert st['members'][3]['valid'] is False and st['members'][4]['up'] is False
assert 'used' not in st['members'][4]
assert st['usable'] == {'used': 1048576, 'free': 2146435072, 'capacity': 2147483648}
assert st['state'] == 'DEGRADED'
assert b.parse_status('Cluster nodes (0):\n')['usable'] is None
# Invalid capacity: quota and quota - used, as efs-mgmt models it.
assert b.member_caps({'valid': False, 'quota': 100, 'used': 30, 'capacity': 7, 'available': 7}) == (100, 70)
assert b.member_caps({'valid': True, 'quota': 100, 'used': 30, 'capacity': 50, 'available': 20}) == (50, 20)

RAFT = '''raft-status 127.0.0.1:40002 node=2
  group 0 hosted=1 role=leader leader=1 term=3 commit=9 applied=9 voters=0x7 joint=0 retired=0 last=9
  group 2 hosted=1 role=follower leader=2 term=2 commit=9 applied=9 voters=0xe joint=0 retired=0 last=9
'''
assert b.parse_voters(RAFT) == {0: (True, 'leader', [1, 2, 3]), 2: (True, 'follower', [2, 3, 4])}
# Node 1's retired group-2 replica still shows {1,2,3}; it does not count.
RETIRED = RAFT.replace('voters=0xe joint=0 retired=0', 'voters=0x7 joint=0 retired=1')
assert b.parse_voters(RETIRED)[2] == (False, 'follower', [1, 2, 3])
LEADER = RAFT.replace('role=leader', 'role=follower').replace('role=follower leader=2', 'role=leader leader=2')
assert b.group_voters([b.parse_voters(RETIRED), b.parse_voters(RAFT)]) == {0: [1, 2, 3], 2: [2, 3, 4]}
assert b.group_voters([b.parse_voters(RETIRED)]) == {0: [1, 2, 3]}

CHUNKS = '''raft-getchunks status=0 primary=2 ino=81927 count=3 cis=0,1,2
  ci=0 nodes=1,2,3 base_gen=0 obj_gen=11 ck0=0a0b0c0d spans=0 seq=0
  ci=1 nodes=2,3,4 base_gen=0 obj_gen=12 ck0=01020304 spans=0 seq=0
  ci=2 nodes=3,4,1 base_gen=0 obj_gen=13 ck0=ffffffff spans=1 seq=4
    span off=0 len=17 gen=14 seq=4 nodes=3,4,1
'''
s, rows = b.parse_getchunks(CHUNKS)
assert s == 0 and [r['ci'] for r in rows] == [0, 1, 2] and rows[1]['nodes'] == [2, 3, 4]
assert rows[2]['obj_gen'] == 13 and rows[2]['spans'] == 1 and rows[0]['packed'] is None
PACKED = '''raft-getchunks status=0 primary=2 ino=9 count=1 cis=0
  ci=0 nodes=4,1,2 base_gen=0 obj_gen=77 ck0=00000001 spans=0 seq=0
    packed segment=1234 gen=1 group=2 offset=4096 len=900
'''
s, prow = b.parse_getchunks(PACKED)
assert prow[0]['packed'] == {'segment': 1234, 'gen': 1, 'group': 2, 'offset': 4096, 'len': 900}
assert b.parse_getchunks('Failed to raft-getchunks\n') == (None, [])
# A partial chunk: a stub base (nodes 0,0,0) and one span object.
SPAN = '''raft-getchunks status=0 primary=3 ino=16385 count=2 cis=0,1
  ci=0 nodes=3,1,2 base_gen=55 obj_gen=55 ck0=f97d765b spans=0 seq=0
  ci=1 nodes=0,0,0 base_gen=0 obj_gen=0 ck0=00000000 spans=1 seq=1
    span off=0 len=1 gen=66 seq=1 nodes=1,2,3
'''
s, srows = b.parse_getchunks(SPAN)
assert srows[1]['span'] == [{'off': 0, 'len': 1, 'gen': 66, 'seq': 1, 'nodes': [1, 2, 3]}], srows
assert b.stub_base(srows[1]) and not b.stub_base(srows[0])
assert b.row_objects(srows[1]) == [(66, [1, 2, 3])] and b.row_objects(srows[0]) == [(55, [3, 1, 2])]
assert b.row_stripe(srows[1]) == [1, 2, 3]

members = {1: {}, 2: {}, 3: {}, 4: {}}
C = 131072
assert b.check_rows('f', 3 * C, rows, members, C) == []
assert b.check_rows('p', 900, prow, members, C) == []
# Negative controls: duplicate and non-member nodes, a missing or extra
# chunk row, a packed row of the wrong length, two rows for a packed file.
dup = [dict(rows[0], nodes=[1, 1, 3])]
assert any('stripe' in m for m in b.check_rows('f', C, dup, members, C))
alien = [dict(rows[0], nodes=[1, 2, 9])]
assert any('stripe' in m for m in b.check_rows('f', C, alien, members, C))
assert b.check_rows('f', 4 * C, rows, members, C) and b.check_rows('f', 2 * C, rows, members, C)
assert b.check_rows('f', 3 * C - 1, rows, members, C) == []
assert b.check_rows('f', 3 * C + 1, rows, members, C)
assert b.check_rows('p', 901, prow, members, C)
assert b.check_rows('p', 900, prow + [dict(rows[1])], members, C)
assert b.check_rows('e', 0, [], members, C) == []
assert b.check_rows('s', C + 1, srows, members, C) == []
# A stub with no span, a span on a duplicate node, a span count mismatch.
assert any('no base and no span' in m for m in b.check_rows('s', C + 1, [srows[0], dict(srows[1], span=[], spans=0)], members, C))
badspan = dict(srows[1], span=[dict(srows[1]['span'][0], nodes=[1, 1, 2])])
assert any('stripe' in m for m in b.check_rows('s', C + 1, [srows[0], badspan], members, C))
assert any('spans' in m for m in b.check_rows('s', C + 1, [srows[0], dict(srows[1], spans=2)], members, C))

# capacity_logical matches efs_capacity_logical's documented cases.
G = 1 << 30
assert b.capacity_logical([G, G, G]) == 2 * G
assert b.capacity_logical([3 * G] * 4) == 8 * G
assert b.capacity_logical([100, 100, 1000]) == 200
assert b.capacity_logical([G, G, 0]) == 0 and b.capacity_logical([G, G]) == 0
assert b.capacity_logical([G, G, G, 0]) == 2 * G


def frag(path, kind, space, fi, obj, gen, ci, plen, direct):
    rec = b.MAGIC + struct.pack('>HHIIIQQIII', 2, kind, 128, space, fi, obj, gen, ci, plen, 1)
    rec += bytes(128 - len(rec))
    tail = rec + bytes(4096 - 128) if direct else rec
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(plen) + tail)


with tempfile.TemporaryDirectory(prefix='efs-b-checks-') as d:
    d = Path(d)
    # Rows of ino 81927 (3 chunks) and one packed file in segment 1234.
    for fi, nid in enumerate((1, 2, 3)):
        frag(d / f'n{nid}/data/exports/1/a/0.{fi}.11', 1, 1, fi, 81927, 11, 0, 65536, nid == 2)
    for fi, nid in enumerate((2, 3, 4)):
        frag(d / f'n{nid}/data/exports/1/a/1.{fi}.12', 1, 1, fi, 81927, 12, 1, 65536, False)
    for fi, nid in enumerate((3, 4, 1)):
        frag(d / f'n{nid}/data/exports/1/a/2.{fi}.13', 1, 1, fi, 81927, 13, 2, 65536, True)
    for fi, nid in enumerate((4, 1, 2)):
        frag(d / f'n{nid}/data/segments/g2/004/{1234:016x}.1.{fi}', 2, 2, fi, 1234, 1, 0, 8192, False)
    for fi, nid in enumerate((3, 4, 1)):
        frag(d / f'n{nid}/data/exports/1/a/2.{fi}.14', 1, 1, fi, 81927, 14, 2, 65536, False)
    for fi, nid in enumerate((3, 1, 2)):
        frag(d / f'n{nid}/data/exports/1/b/0.{fi}.55', 1, 1, fi, 16385, 55, 0, 65536, True)
    for fi, nid in enumerate((1, 2, 3)):
        frag(d / f'n{nid}/data/exports/1/b/1.{fi}.66', 1, 1, fi, 16385, 66, 1, 65536, True)
    (d / 'n1/data/exports/1/a/junk').write_bytes(b'not a fragment' * 20)
    inv = {nid: b.store_inventory(d / f'n{nid}') for nid in (1, 2, 3, 4)}
    assert sum(len(v) for v in inv.values()) == 21, inv
    assert any(r['kind'] == 2 and r['object'] == 1234 for r in inv[4])
    assert all(r['plen'] in (65536, 8192) for v in inv.values() for r in v)
    by_file = {('f', 81927): rows, ('p', 9): prow, ('s', 16385): srows}
    bad, checked = b.inventory_check(by_file, inv)
    assert bad == [] and checked == 21, (bad, checked)
    # The span's fragment missing on its node is found.
    nospan = {k: [r for r in v if not (r['object'] == 16385 and r['gen'] == 66 and r['fi'] == 2)] for k, v in inv.items()}
    bad, _ = b.inventory_check(by_file, nospan)
    assert len(bad) == 1 and 'gen=66 role 2 not on node 3' in bad[0], bad
    # Negative controls: a missing fragment, a stray copy on another node,
    # a record of the wrong generation, a store not readable here.
    miss = {k: [r for r in v if not (k == 2 and r['ci'] == 1 and r['kind'] == 1 and r['object'] == 81927)] for k, v in inv.items()}
    bad, _ = b.inventory_check(by_file, miss)
    assert len(bad) == 1 and 'role 0 not on node 2' in bad[0], bad
    stray = {k: list(v) for k, v in inv.items()}
    stray[4] = stray[4] + [dict(inv[1][0])]
    bad, _ = b.inventory_check(by_file, stray)
    assert any('also on [4]' in m for m in bad), bad
    regen = [dict(rows[0], obj_gen=99)] + rows[1:]
    bad, _ = b.inventory_check({('f', 81927): regen, ('p', 9): prow}, inv)
    assert sum('not on node' in m for m in bad) == 3, bad
    part = {k: v for k, v in inv.items() if k != 3}
    bad, checked = b.inventory_check(by_file, part)
    assert bad == [] and checked == 15, (bad, checked)
    # Costs from the records: 3 x 64 KiB per chunk row; a segment's three
    # fragments shared by the files in it.
    cost = b.file_costs(by_file, inv)
    assert cost['f']['fragments'] == 12 and cost['f']['payload'] == 12 * 65536, cost
    assert cost['f']['allocated'] >= cost['f']['payload'] or cost['f']['allocated'] == 0
    assert cost['p']['fragments'] == 3 and cost['p']['payload'] == 3 * 8192, cost
    # A 1-byte tail chunk costs a whole stripe (the span's three fragments).
    assert cost['s']['fragments'] == cost['s']['expected'] == 6 and cost['s']['payload'] == 6 * 65536, cost
    two = dict(by_file)
    two[('q', 10)] = prow
    cost = b.file_costs(two, inv)
    assert cost['p']['payload'] == cost['q']['payload'] == 3 * 8192 / 2, cost
    q3 = [dict(prow[0], packed=dict(prow[0]['packed'], len=2700))]
    cost = b.file_costs({('p', 9): prow, ('q', 10): q3}, inv)
    assert cost['p']['payload'] == 3 * 8192 * 900 / 3600 and cost['q']['payload'] == 3 * 8192 * 2700 / 3600, cost
    assert b.file_costs(by_file, miss)['f']['fragments'] == 11
    # A truncated or foreign file is not a record.
    assert b.read_record(d / 'n1/data/exports/1/a/junk') is None
    assert b.parse_record(b'EFSFRAG\0' + bytes(120)) is None

print('b_checks driver: status/raft-status/getchunks parsing, row shape, capacity model and'
      ' persisted-inventory checks with negative controls PASS')
