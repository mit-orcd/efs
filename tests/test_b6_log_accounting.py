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
node = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'packed_total')
namespace = {'re': re}
exec(compile(ast.Module(body=[node], type_ignores=[]), str(source), 'exec'), namespace)
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
print('B6 packer process counter: group/term changes do not invent packs; restarts and members sum PASS')
