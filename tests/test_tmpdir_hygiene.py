#!/usr/bin/env python3
"""K25: `make test` scratch honours TMPDIR and is removed on success.

Unit tests hard-coded /tmp; on the build VMs /tmp is a small tmpfs, and
directories the tests never removed filled it. C tests now build scratch
paths with tests/test_tmpdir.h (efs_test_path + efs_test_mkdtemp/mkstemp/own),
which places them under $TMPDIR (default /tmp), removes them when the test
exits 0 and prints "efs-test: kept <path>" otherwise. This static check keeps
new tests on that path:

- no "/tmp/..." string literal in tests/*.c or tests/faults/*.c (literals
  that are only path-normalisation inputs are listed below);
- no bare mkdtemp()/mkstemp() there (the wrappers register cleanup);
- no mkdtemp-style "/tmp/...XXXXXX" template or dir='/tmp' in the Python
  tests (embedded C included) and no `mktemp /tmp/...` in the Makefile.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
# File -> literals allowed because they are data, not scratch locations.
ALLOWED = {
    'tests/test_stop_control.c': {'"/tmp/nonexistent/../efs-target/"', '"/tmp/./efs-target"'},
}
bad = []
for p in sorted(list(root.glob('tests/*.c')) + list(root.glob('tests/faults/*.c'))):
    rel = str(p.relative_to(root)); text = p.read_text()
    for lit in re.findall(r'"/tmp[^"]*"', text):
        if lit not in ALLOWED.get(rel, ()):
            bad.append('%s: hard-coded %s (use efs_test_path)' % (rel, lit))
    for m in re.finditer(r'(?<![\w.>])(mkdtemp|mkstemp)\(', text):
        bad.append('%s: bare %s() (use efs_test_%s so the path is removed on success)' % (rel, m.group(1), m.group(1)))
for p in sorted(list(root.glob('tests/test_*.py')) + list(root.glob('tests/faults/test_*.py'))):
    rel = str(p.relative_to(root)); text = p.read_text()
    if p == Path(__file__).resolve(): continue
    for m in re.finditer(r'/tmp/[\w.-]*XXXXXX', text):
        bad.append('%s: hard-coded template %s' % (rel, m.group(0)))
    for m in re.finditer(r"""dir\s*=\s*['"]/tmp['"]""", text):
        bad.append('%s: %s (tempfile already honours TMPDIR)' % (rel, m.group(0)))
for m in re.finditer(r'mktemp\s+/tmp/\S+', (root / 'Makefile').read_text()):
    bad.append('Makefile: %s (use "$${TMPDIR:-/tmp}")' % m.group(0))
for rel, lits in ALLOWED.items():
    text = (root / rel).read_text()
    stale = [l for l in lits if l not in text]
    assert not stale, 'stale ALLOWED entries in %s: %s' % (rel, stale)
assert not bad, 'test scratch outside TMPDIR or without cleanup (K25):\n  ' + '\n  '.join(bad)
print('test scratch paths: TMPDIR only, removed on success (C, Python, Makefile) PASS')
