#!/usr/bin/env python3
"""B1: the client reader takes fragment nodes only from the chunk row (or
the pack object) and never recomputes placement. Before Oct 24 a row whose
fragment nodes were all zero, or a missing row, was decoded from
efs_place_fragments() under the current membership, which after a node
addition names other members than the ones that hold the fragments.
Static check of src/client/read.c; efs_fragment_nodes_valid() itself is
tested in tests/test_b_geometry.c and the persisted rows by
tests/test_b_geometry.py."""
import re
from pathlib import Path

s = (Path(__file__).resolve().parents[1] / 'src/client/read.c').read_text()
code = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
assert not re.search(r'\befs_(place_fragments|get_placement|pack_destinations)\s*\(', code), \
    'read.c recomputes placement'
a = code.index('static int efs_client_decode_placed_chunk_attempts(')
body = code[a:code.index('\n}', a)]
gate = body.find('if (!have_ce || !efs_fragment_nodes_valid(ce.fragment_nodes))')
assert gate > 0, 'decode does not gate the recorded stripe'
assert body[gate:].lstrip().startswith('if (!have_ce') and 'return EFS_ERR_PROTO;' in body[gate:gate + 160]
assert gate < body.index('get_two_parallel(jobs)'), 'a GET can run before the stripe gate'
assert gate < body.index('memcpy(nodes, ce.fragment_nodes, sizeof(nodes));')
# The packed path decodes from the row's (pack object's) nodes, validated
# inside efs_packed_read_base_scratch.
assert 'efs_packed_read_base_scratch(&ce.packed, ce.fragment_nodes,' in body
pr = (Path(__file__).resolve().parents[1] / 'src/common/packed_reader.c').read_text()
assert 'if (!nodes[i]) return EFS_ERR_INVAL;' in pr and 'if (nodes[i] == nodes[j]) return EFS_ERR_INVAL;' in pr
print('B1 reader: fragment nodes only from the row or pack object, invalid stripe refused before any GET PASS')
