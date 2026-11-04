#!/usr/bin/env python3
"""Fail when src/ gains a cross-directory `#include "../<dir>/..."` (W171 phase 3).

Usage: python3 scripts/check_include_boundary.py [--selftest]
Includes are counted per (including src dir, included src dir) pair, so moving
or splitting files inside one src/<dir> never changes the result. Exit 0 = PASS.
"""
import re
import sys
import tempfile
from collections import Counter
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / 'src'
# Allowed includes per (including dir, included dir); lower the number when one goes away.
ALLOWED = {
    ('bench', 'common'): 1, ('bench', 'server'): 1,
    ('client', 'bench'): 4, ('client', 'common'): 8,
    ('meta', 'common'): 2,
    ('mgmt', 'client'): 1, ('mgmt', 'common'): 1,
    ('server', 'common'): 4,
}
INCLUDE = re.compile(r'^\s*#\s*include\s+"((?:\.\./)+[^"]+)"', re.M)


def scan(src):
    """Return {(from_dir, to_dir): [file:line, ...]} for includes that leave their src dir."""
    found = {}
    for path in sorted(p for p in src.rglob('*') if p.suffix in ('.c', '.h')):
        text = path.read_text(errors='replace')
        for m in INCLUDE.finditer(text):
            parts = (path.parent / m.group(1)).resolve().relative_to(src.resolve()).parts
            here = path.relative_to(src).parts[0]
            if len(parts) > 1 and parts[0] != here:
                line = text.count('\n', 0, m.start()) + 1
                found.setdefault((here, parts[0]), []).append(f'{path.relative_to(src)}:{line}')
    return found


def check(src, allowed):
    found = scan(src)
    bad = [f'  {w}: new include of {b}/ from {a}/' for (a, b), ws in sorted(found.items())
           for w in ws[allowed.get((a, b), 0):]]
    tight = [f'{a}->{b} allows {n}, has {len(found.get((a, b), []))}'
             for (a, b), n in sorted(allowed.items()) if len(found.get((a, b), [])) < n]
    total = sum(map(len, found.values()))
    return bad, tight, f'include boundary: {total} includes, {len(found)} pairs'


def selftest():
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        for sub in ('a/sub', 'b', 'c'):
            (root / sub).mkdir(parents=True)
        (root / 'a/x.c').write_text('#include "../b/y.h"\n#include "local.h"\n')
        (root / 'a/sub/z.c').write_text('#include "../../b/y.h"\n#include "../x.h"\n')
        assert check(root, {('a', 'b'): 2})[:2] == ([], [])      # subdir resolves to dir a
        bad, tight, _ = check(root, {('a', 'b'): 1})              # count above allowlist
        assert len(bad) == 1, bad
        (root / 'c/n.c').write_text('#include "../a/x.h"\n')       # new pair
        assert len(check(root, {('a', 'b'): 2})[0]) == 1
        assert check(root, {('a', 'b'): 3, ('c', 'a'): 1})[1] == ['a->b allows 3, has 2']
    print('PASS include boundary selftest')


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        selftest()
        sys.exit(0)
    if sys.argv[1:]:
        sys.exit(__doc__)
    bad, tight, summary = check(SRC, ALLOWED)
    print('\n'.join(bad + [f'  can tighten: {t}' for t in tight]))
    print(('FAIL ' if bad else 'PASS ') + summary)
    sys.exit(1 if bad else 0)
