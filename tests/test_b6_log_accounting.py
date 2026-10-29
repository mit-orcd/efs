#!/usr/bin/env python3
"""The packer counter is per process, not per group or leader term.

Read the actual B6 counting function without starting a cluster. An optional
source path supplies the pre-fix negative control; only synthetic logs enter.
"""
import ast
from pathlib import Path
import re
import sys

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name('test_b6_unavailable.py')
tree = ast.parse(source.read_text())
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in
         ('packed_total', 'completed_pack_log', 'degraded_writes')]
namespace = {'re': re}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(source), 'exec'), namespace)
count = namespace['packed_total']

def line(group, term, total):
    return f'packer: group={group} term={term} candidates=4 packed_total={total}\n'

before = '==== start n1\n' + line(0, 1, 4)
# Merely logging the same process-wide total from a different group/term
# is NOT another four files packed while a destination was unavailable.
for after in (before + line(2, 1, 4), before + line(0, 2, 4),
              before + line(2, 2, 4) + line(0, 3, 4)):
    assert count(None, {1: after}) == count(None, {1: before}) == 4
assert count(None, {1: before + line(2, 1, 6) + line(0, 2, 6)}) == 6
# Restart resets that process's counter; another member is independent.
restarted = before + '==== start n1\n' + line(2, 3, 3) + line(0, 4, 3)
assert count(None, {1: restarted, 2: '==== start n2\n' + line(2, 7, 2)}) == 9
assert count(None, {1: '==== start n1\n'}) == 0
cut = namespace['completed_pack_log']
degraded = namespace['degraded_writes']
mark = ('packer: segment g0 id=0000000000000123 gen=1 written degraded: '
        'fragment 2 for node 3 not acknowledged (rc=-1); marked for rebuild on return\n')
# The real failure window: one snapshot contains the mark but not the
# counter increment. The next snapshot must count both as new, not invent
# four newly packed files with no newly written degraded segment.
pending = before + mark
after = pending + line(0, 1, 8)
assert count(None, {1: cut(pending)}) == 4
assert degraded(None, {1: cut(pending)}) == []
assert count(None, {1: cut(after)}) == 8
assert degraded(None, {1: cut(after)}) == [('0', '0000000000000123', '1', '2', '3')]
# A short read of the summary itself cannot close the pass. A restart
# without its first completed pass must not reuse an earlier run's counter.
assert cut(pending + line(0, 1, 8).rstrip('\n')) == cut(pending)
assert count(None, {1: cut(after + '==== start n1\n' + mark)}) == 8
assert count(None, {1: cut(after + '==== start n1\n' + mark + line(2, 3, 2))}) == 10
print('B6 log accounting: completed-pass snapshots pair marks/counters; partial lines, group/term changes and restarts PASS')
